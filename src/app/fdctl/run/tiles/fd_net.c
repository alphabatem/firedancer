#include "../../../../disco/tiles.h"

/* The net tile translates between AF_XDP and fd_tango
   traffic.  It is responsible for setting up the XDP and
   XSK socket configuration.

   ### Why does this tile bind to loopback?

   The Linux kernel does some short circuiting optimizations
   when sending packets to an IP address that's owned by the
   same host. The optimization is basically to route them over
   to the loopback interface directly, bypassing the network
   hardware.

   This redirection to the loopback interface happens before
   XDP programs are executed, so local traffic destined for
   our listen addresses will not get ingested correctly.

   There are two reasons we send traffic locally,

   * For testing and development.
   * The Agave code sends local traffic to itself to
     as part of routine operation (eg, when it's the leader
     it sends votes to its own TPU socket).

   So for now we need to also bind to loopback. This is a
   small performance hit for other traffic, but we only
   redirect packets destined for our target IP and port so
   it will not otherwise interfere. Loopback only supports
   XDP in SKB mode. */

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sys/socket.h> /* MSG_DONTWAIT needed before importing the net seccomp filter */
#include <linux/if_xdp.h>

#include "generated/net_seccomp.h"

#include "../../../../disco/metrics/fd_metrics.h"

#include "../../../../waltz/xdp/fd_xdp.h"
#include "../../../../waltz/xdp/fd_xdp1.h"
#include "../../../../waltz/xdp/fd_xsk_aio_private.h"
#include "../../../../waltz/xdp/fd_xsk_private.h"
#include "../../../../util/log/fd_dtrace.h"
#include "../../../../util/net/fd_ip4.h"
#include "../../../../waltz/ip/fd_ip.h"

#include <unistd.h>
#include <linux/unistd.h>

#define MAX_NET_INS (32UL)

typedef struct {
  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
} fd_net_in_ctx_t;

typedef struct {
  fd_frag_meta_t * mcache;
  ulong *          sync;
  ulong            depth;
  ulong            seq;

  fd_wksp_t * mem;
  ulong       chunk0;
  ulong       wmark;
  ulong       chunk;
} fd_net_out_ctx_t;

typedef struct {
  ulong          xsk_cnt;
  fd_xsk_t *     xsk[ 2 ];
  fd_xsk_aio_t * xsk_aio[ 2 ];
  int            prog_link_fds[ 2 ];

  ulong round_robin_cnt;
  ulong round_robin_id;

  const fd_aio_t * tx;
  const fd_aio_t * lo_tx;

  uchar frame[ FD_NET_MTU ];

  uint   src_ip_addr;
  uchar  src_mac_addr[6];

  ushort shred_listen_port;
  ushort quic_transaction_listen_port;
  ushort legacy_transaction_listen_port;
  ushort gossip_listen_port;
  ushort repair_intake_listen_port;
  ushort repair_serve_listen_port;

  ulong in_cnt;
  fd_net_in_ctx_t in[ MAX_NET_INS ];

  fd_net_out_ctx_t quic_out[1];
  fd_net_out_ctx_t shred_out[1];
  fd_net_out_ctx_t gossip_out[1];
  fd_net_out_ctx_t repair_out[1];

  fd_ip_t *   ip;
  long        ip_next_upd;

  struct {
    ulong tx_dropped_cnt;
  } metrics;
} fd_net_ctx_t;

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return 4096UL;
}

FD_FN_PURE static inline ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  /* TODO reproducing this conditional memory layout twice is susceptible to bugs. Use more robust object discovery */
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_net_ctx_t), sizeof(fd_net_ctx_t) );
  l = FD_LAYOUT_APPEND( l, fd_aio_align(),        fd_aio_footprint() );
  l = FD_LAYOUT_APPEND( l, fd_xsk_align(),        fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
  l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(),    fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  if( FD_UNLIKELY( strcmp( tile->net.interface, "lo" ) && tile->kind_id == 0 ) ) {
    l = FD_LAYOUT_APPEND( l, fd_xsk_align(),      fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) );
    l = FD_LAYOUT_APPEND( l, fd_xsk_aio_align(),  fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) );
  }
  l = FD_LAYOUT_APPEND( l, fd_ip_align(),         fd_ip_footprint( 0UL, 0UL ) );
  return FD_LAYOUT_FINI( l, scratch_align() );
}

/* net_rx_aio_send is a callback invoked by aio when new data is
   received on an incoming xsk.  The xsk might be bound to any interface
   or ports, so the purpose of this callback is to determine if the
   packet might be a valid transaction, and whether it is QUIC or
   non-QUIC (raw UDP) before forwarding to the appropriate handler.

   This callback is supposed to return the number of packets in the
   batch which were successfully processed, but we always return
   batch_cnt since there is no logic in place to backpressure this far
   up the stack, and there is no sane way to "not handle" an incoming
   packet. */
static int
net_rx_aio_send( void *                    _ctx,
                 fd_aio_pkt_info_t const * batch,
                 ulong                     batch_cnt,
                 ulong *                   opt_batch_idx,
                 int                       flush ) {
  (void)flush;

  fd_net_ctx_t * ctx = (fd_net_ctx_t *)_ctx;

  for( ulong i=0UL; i<batch_cnt; i++ ) {
    uchar const * packet = batch[i].buf;
    uchar const * packet_end = packet + batch[i].buf_sz;

    if( FD_UNLIKELY( batch[i].buf_sz > FD_NET_MTU ) )
      FD_LOG_ERR(( "received a UDP packet with a too large payload (%u)", batch[i].buf_sz ));

    uchar const * iphdr = packet + 14U;

    /* Filter for UDP/IPv4 packets. Test for ethtype and ipproto in 1
       branch */
    uint test_ethip = ( (uint)packet[12] << 16u ) | ( (uint)packet[13] << 8u ) | (uint)packet[23];
    if( FD_UNLIKELY( test_ethip!=0x080011 ) )
      FD_LOG_ERR(( "Firedancer received a packet from the XDP program that was either "
                   "not an IPv4 packet, or not a UDP packet. It is likely your XDP program "
                   "is not configured correctly." ));

    /* IPv4 is variable-length, so lookup IHL to find start of UDP */
    uint iplen = ( ( (uint)iphdr[0] ) & 0x0FU ) * 4U;
    uchar const * udp = iphdr + iplen;

    /* Ignore if UDP header is too short */
    if( FD_UNLIKELY( udp+8U > packet_end ) ) {
      FD_DTRACE_PROBE( net_tile_err_rx_undersz );
      continue;
    }

    /* Extract IP dest addr and UDP src/dest port */
    uint ip_srcaddr    =                  *(uint   *)( iphdr+12UL );
    ushort udp_srcport = fd_ushort_bswap( *(ushort *)( udp+0UL    ) );
    ushort udp_dstport = fd_ushort_bswap( *(ushort *)( udp+2UL    ) );

    FD_DTRACE_PROBE_4( net_tile_pkt_rx, ip_srcaddr, udp_srcport, udp_dstport, batch[i].buf_sz );

    ushort proto;
    fd_net_out_ctx_t * out;
    if(      FD_UNLIKELY( udp_dstport==ctx->shred_listen_port ) ) {
      proto = DST_PROTO_SHRED;
      out = ctx->shred_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->quic_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_QUIC;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->legacy_transaction_listen_port ) ) {
      proto = DST_PROTO_TPU_UDP;
      out = ctx->quic_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->gossip_listen_port ) ) {
      proto = DST_PROTO_GOSSIP;
      out = ctx->gossip_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_intake_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else if( FD_UNLIKELY( udp_dstport==ctx->repair_serve_listen_port ) ) {
      proto = DST_PROTO_REPAIR;
      out = ctx->repair_out;
    } else {

      FD_LOG_ERR(( "Firedancer received a UDP packet on port %hu which was not expected. "
                   "Only the following ports should be configured to forward packets: "
                   "%hu, %hu, %hu, %hu, %hu, %hu (excluding any 0 ports, which can be ignored)."
                   "It is likely you changed the port configuration in your TOML file and "
                   "did not reload the XDP program. You can reload the program by running "
                   "`fdctl configure fini xdp && fdctl configure init xdp`.",
                   udp_dstport,
                   ctx->shred_listen_port,
                   ctx->quic_transaction_listen_port,
                   ctx->legacy_transaction_listen_port,
                   ctx->gossip_listen_port,
                   ctx->repair_intake_listen_port,
                   ctx->repair_serve_listen_port ));
    }

    fd_memcpy( fd_chunk_to_laddr( out->mem, out->chunk ), packet, batch[ i ].buf_sz );

    /* tile can decide how to partition based on src ip addr and src port */
    ulong sig = fd_disco_netmux_sig( ip_srcaddr, udp_srcport, 0U, proto, 14UL+8UL+iplen );

    ulong tspub  = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );
    fd_mcache_publish( out->mcache, out->depth, out->seq, sig, out->chunk, batch[ i ].buf_sz, 0, 0, tspub );

    out->seq = fd_seq_inc( out->seq, 1UL );
    out->chunk = fd_dcache_compact_next( out->chunk, FD_NET_MTU, out->chunk0, out->wmark );
  }

  if( FD_LIKELY( opt_batch_idx ) ) {
    *opt_batch_idx = batch_cnt;
  }

  return FD_AIO_SUCCESS;
}

static void
metrics_write( fd_net_ctx_t * ctx ) {
  ulong rx_cnt = ctx->xsk_aio[ 0 ]->metrics.rx_cnt;
  ulong rx_sz  = ctx->xsk_aio[ 0 ]->metrics.rx_sz;
  ulong tx_cnt = ctx->xsk_aio[ 0 ]->metrics.tx_cnt;
  ulong tx_sz  = ctx->xsk_aio[ 0 ]->metrics.tx_sz;
  if( FD_LIKELY( ctx->xsk_aio[ 1 ] ) ) {
    rx_cnt += ctx->xsk_aio[ 1 ]->metrics.rx_cnt;
    rx_sz  += ctx->xsk_aio[ 1 ]->metrics.rx_sz;
    tx_cnt += ctx->xsk_aio[ 1 ]->metrics.tx_cnt;
    tx_sz  += ctx->xsk_aio[ 1 ]->metrics.tx_sz;
  }

  FD_MCNT_SET( NET, RECEIVED_PACKETS, rx_cnt );
  FD_MCNT_SET( NET, RECEIVED_BYTES,   rx_sz  );
  FD_MCNT_SET( NET, SENT_PACKETS,     tx_cnt );
  FD_MCNT_SET( NET, SENT_BYTES,       tx_sz  );

  FD_MCNT_SET( NET, TX_DROPPED, ctx->metrics.tx_dropped_cnt );
}

static void
before_credit( fd_net_ctx_t *      ctx,
               fd_stem_context_t * stem,
               int *               charge_busy ) {
  (void)stem;

  for( ulong i=0UL; i<ctx->xsk_cnt; i++ ) {
    if( FD_LIKELY( fd_xsk_aio_service( ctx->xsk_aio[i] ) ) ) {
      *charge_busy = 1;
    }
  }
}

struct xdp_statistics_v0 {
  __u64 rx_dropped; /* Dropped for other reasons */
  __u64 rx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 tx_invalid_descs; /* Dropped due to invalid descriptor */
};

struct xdp_statistics_v1 {
  __u64 rx_dropped; /* Dropped for other reasons */
  __u64 rx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 tx_invalid_descs; /* Dropped due to invalid descriptor */
  __u64 rx_ring_full; /* Dropped due to rx ring being full */
  __u64 rx_fill_ring_empty_descs; /* Failed to retrieve item from fill ring */
  __u64 tx_ring_empty_descs; /* Failed to retrieve item from tx ring */
};

static inline void
poll_xdp_statistics( fd_net_ctx_t * ctx ) {
  struct xdp_statistics_v1 stats;
  uint optlen = (uint)sizeof(stats);
  if( FD_UNLIKELY( -1==getsockopt( ctx->xsk[ 0 ]->xsk_fd, SOL_XDP, XDP_STATISTICS, &stats, &optlen ) ) )
    FD_LOG_ERR(( "getsockopt(SOL_XDP, XDP_STATISTICS) failed: %s", strerror( errno ) ));

  if( FD_LIKELY( optlen==sizeof(struct xdp_statistics_v1) ) ) {
    FD_MCNT_SET( NET, XDP_RX_DROPPED_OTHER, stats.rx_dropped );
    FD_MCNT_SET( NET, XDP_RX_DROPPED_RING_FULL, stats.rx_ring_full );

    FD_TEST( !stats.rx_invalid_descs );
    FD_TEST( !stats.tx_invalid_descs );
    /* TODO: We shouldn't ever try to tx or rx with empty descs but we
             seem to sometimes. */
    // FD_TEST( !stats.rx_fill_ring_empty_descs );
    // FD_TEST( !stats.tx_ring_empty_descs );
  } else if( FD_LIKELY( optlen==sizeof(struct xdp_statistics_v0) ) ) {
    FD_MCNT_SET( NET, XDP_RX_DROPPED_OTHER, stats.rx_dropped );

    FD_TEST( !stats.rx_invalid_descs );
    FD_TEST( !stats.tx_invalid_descs );
  } else {
    FD_LOG_ERR(( "getsockopt(SOL_XDP, XDP_STATISTICS) returned unexpected size %u", optlen ));
  }
}

static void
during_housekeeping( fd_net_ctx_t * ctx ) {
  long now = fd_log_wallclock();
  if( FD_UNLIKELY( now > ctx->ip_next_upd ) ) {
    ctx->ip_next_upd = now + (long)60e9;
    fd_ip_arp_fetch( ctx->ip );
    fd_ip_route_fetch( ctx->ip );
  }

  /* Only net tile 0 polls the statistics, as they are retrieved for the
     XDP socket which is shared across all net tiles. */

  if( FD_LIKELY( !ctx->round_robin_id ) ) poll_xdp_statistics( ctx );
}

FD_FN_PURE static int
route_loopback( uint  tile_ip_addr,
                ulong sig ) {
  return fd_disco_netmux_sig_dst_ip( sig )==FD_IP4_ADDR(127,0,0,1) ||
    fd_disco_netmux_sig_dst_ip( sig )==tile_ip_addr;
}

static inline int
before_frag( fd_net_ctx_t * ctx,
             ulong          in_idx,
             ulong          seq,
             ulong          sig ) {
  (void)in_idx;

  ulong proto = fd_disco_netmux_sig_proto( sig );
  if( FD_UNLIKELY( proto!=DST_PROTO_OUTGOING ) ) return 1;

  /* Round robin by sequence number for now, QUIC should be modified to
     echo the net tile index back so we can transmit on the same queue.

     127.0.0.1 packets for localhost must go out on net tile 0 which
     owns the loopback interface XSK, which only has 1 queue. */

  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, sig ) ) ) return ctx->round_robin_id != 0UL;
  else                                                         return (seq % ctx->round_robin_cnt) != ctx->round_robin_id;
}

static inline void
during_frag( fd_net_ctx_t * ctx,
             ulong          in_idx,
             ulong          seq,
             ulong          sig,
             ulong          chunk,
             ulong          sz ) {
  (void)in_idx;
  (void)seq;
  (void)sig;

  if( FD_UNLIKELY( chunk<ctx->in[ in_idx ].chunk0 || chunk>ctx->in[ in_idx ].wmark || sz>FD_NET_MTU ) )
    FD_LOG_ERR(( "chunk %lu %lu corrupt, not in range [%lu,%lu]", chunk, sz, ctx->in[ in_idx ].chunk0, ctx->in[ in_idx ].wmark ));

  uchar * src = (uchar *)fd_chunk_to_laddr( ctx->in[ in_idx ].mem, chunk );
  fd_memcpy( ctx->frame, src, sz ); // TODO: Change xsk_aio interface to eliminate this copy
}

static void
send_arp_probe( fd_net_ctx_t * ctx,
                uint           dst_ip_addr,
                uint           ifindex ) {
  uchar          arp_buf[FD_IP_ARP_SZ];
  ulong          arp_len = 0UL;

  uint           src_ip_addr  = ctx->src_ip_addr;
  uchar *        src_mac_addr = ctx->src_mac_addr;

  /* prepare arp table */
  int arp_table_rtn = fd_ip_update_arp_table( ctx->ip, dst_ip_addr, ifindex );

  if( FD_UNLIKELY( arp_table_rtn == FD_IP_SUCCESS ) ) {
    /* generate a probe */
    fd_ip_arp_gen_arp_probe( arp_buf, FD_IP_ARP_SZ, &arp_len, dst_ip_addr, fd_uint_bswap( src_ip_addr ), src_mac_addr );

    /* send the probe */
    fd_aio_pkt_info_t aio_buf = { .buf = arp_buf, .buf_sz = (ushort)arp_len };
    ulong sent_cnt;
    int aio_err = ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, &sent_cnt, 1 );
    ctx->metrics.tx_dropped_cnt += aio_err!=FD_AIO_SUCCESS;
  }
}

static void
after_frag( fd_net_ctx_t *      ctx,
            ulong               in_idx,
            ulong               seq,
            ulong               sig,
            ulong               sz,
            ulong               tsorig,
            fd_stem_context_t * stem ) {
  (void)in_idx;
  (void)seq;
  (void)sig;
  (void)tsorig;
  (void)stem;

  fd_aio_pkt_info_t aio_buf = { .buf = ctx->frame, .buf_sz = (ushort)sz };
  if( FD_UNLIKELY( route_loopback( ctx->src_ip_addr, sig ) ) ) {
    /* Set Ethernet src and dst address to 00:00:00:00:00:00 */
    memset( ctx->frame, 0, 12UL );

    ulong sent_cnt;
    int aio_err = ctx->lo_tx->send_func( ctx->xsk_aio[ 1 ], &aio_buf, 1, &sent_cnt, 1 );
    ctx->metrics.tx_dropped_cnt += aio_err!=FD_AIO_SUCCESS;
  } else {
    /* extract dst ip */
    uint dst_ip = fd_uint_bswap( fd_disco_netmux_sig_dst_ip( sig ) );

    uint  next_hop    = 0U;
    uchar dst_mac[6]  = {0};
    uint  if_idx      = 0;

    /* route the packet */
    /*
     * determine the destination:
     *   same host
     *   same subnet
     *   other
     * determine the next hop
     *   localhost
     *   gateway
     *   subnet local host
     * determine the mac address of the next hop address
     *   and the local ipv4 and eth addresses */
    int rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    if( FD_UNLIKELY( rtn == FD_IP_PROBE_RQD ) ) {
      /* another fd_net instance might have already resolved this address
         so simply try another fetch */
      fd_ip_arp_fetch( ctx->ip );
      rtn = fd_ip_route_ip_addr( dst_mac, &next_hop, &if_idx, ctx->ip, dst_ip );
    }

    long now;
    switch( rtn ) {
      case FD_IP_PROBE_RQD:
        /* TODO possibly buffer some data while waiting for ARPs to complete */
        /* TODO rate limit ARPs */
        /* TODO add caching of ip_dst -> routing info */
        send_arp_probe( ctx, next_hop, if_idx );

        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        break;
      case FD_IP_NO_ROUTE:
        /* cannot make progress here */
        break;
      case FD_IP_SUCCESS:
        /* set destination mac address */
        memcpy( ctx->frame, dst_mac, 6UL );

        /* set source mac address */
        memcpy( ctx->frame + 6UL, ctx->src_mac_addr, 6UL );

        ulong sent_cnt;
        int aio_err = ctx->tx->send_func( ctx->xsk_aio[ 0 ], &aio_buf, 1, &sent_cnt, 1 );
        ctx->metrics.tx_dropped_cnt += aio_err!=FD_AIO_SUCCESS;
        break;
      case FD_IP_RETRY:
        /* refresh tables */
        now = fd_log_wallclock();
        ctx->ip_next_upd = now + (long)200e3;
        /* TODO consider buffering */
        break;
      case FD_IP_MULTICAST:
      case FD_IP_BROADCAST:
      default:
        /* should not occur in current use cases */
        break;
    }
  }
}

/* init_link_session is part of privileged_init.  It only runs on net
   tile 0.  This function does shared pre-configuration used by all
   other net tiles.  This includes installing the XDP program and
   setting up the XSKMAP into which the other net tiles can register
   themselves into.

   session, link_session, lo_session get initialized with session
   objects.  tile points to the net tile's config.  if_idx, lo_idx
   locate the device IDs of the main and loopback interface.
   *xsk_map_fd, *lo_xsk_map_fd are set to the newly created XSKMAP file
   descriptors.

   Note that if the main interface is loopback, then the loopback-
   related structures are uninitialized.

   Kernel object references:

     BPF_LINK file descriptor
      |
      +-> XDP program installation on NIC
      |    |
      |    +-> XDP program <-- BPF_PROG file descriptor (prog_fd)
      |
      +-> XSKMAP object <-- BPF_MAP file descriptor (xsk_map)
      |
      +-> BPF_MAP object <-- BPF_MAP file descriptor (udp_dsts) */

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );

  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_net_ctx_t), sizeof(fd_net_ctx_t) );
                       FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(),        fd_aio_footprint()   );

  uint if_idx = if_nametoindex( tile->net.interface );
  if( FD_UNLIKELY( !if_idx ) ) FD_LOG_ERR(( "if_nametoindex(%s) failed", tile->net.interface ));

  /* Create and install XSKs */

  int xsk_map_fd = 123462;
  ctx->prog_link_fds[ 0 ] = 123463;
  ctx->xsk[ 0 ] =
      fd_xsk_join(
      fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                  FD_NET_MTU,
                  tile->net.xdp_rx_queue_size,
                  tile->net.xdp_rx_queue_size,
                  tile->net.xdp_tx_queue_size,
                  tile->net.xdp_tx_queue_size ) );
  if( FD_UNLIKELY( !ctx->xsk[ 0 ] ) )                                                    FD_LOG_ERR(( "fd_xsk_new failed" ));
  uint flags = tile->net.zero_copy ? XDP_ZEROCOPY : XDP_COPY;
  if( FD_UNLIKELY( !fd_xsk_init( ctx->xsk[ 0 ], if_idx, (uint)tile->kind_id, flags ) ) ) FD_LOG_ERR(( "failed to bind xsk for net tile %lu", tile->kind_id ));
  if( FD_UNLIKELY( !fd_xsk_activate( ctx->xsk[ 0 ], xsk_map_fd ) ) )                     FD_LOG_ERR(( "failed to activate xsk for net tile %lu", tile->kind_id ));

  if( FD_UNLIKELY( fd_sandbox_gettid()==fd_sandbox_getpid() ) ) {
    /* Kind of gross.. in single threaded mode we don't want to close the xsk_map_fd
       since it's shared with other net tiles.  Just check for that by seeing if we
       are the only thread in the process. */
    if( FD_UNLIKELY( -1==close( xsk_map_fd ) ) )                                         FD_LOG_ERR(( "close(%d) failed (%d-%s)", xsk_map_fd, errno, fd_io_strerror( errno ) ));
  }

  ctx->xsk_aio[ 0 ] = fd_xsk_aio_join( fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                                       tile->net.xdp_tx_queue_size,
                                                       tile->net.xdp_aio_depth ), ctx->xsk[ 0 ] );
  if( FD_UNLIKELY( !ctx->xsk_aio[ 0 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_join failed" ));

  /* Networking tile at index 0 also binds to loopback (only queue 0 available on lo) */

  if( FD_UNLIKELY( strcmp( tile->net.interface, "lo" ) && !tile->kind_id ) ) {
    ctx->xsk_cnt = 2;

    ushort udp_port_candidates[] = {
      (ushort)tile->net.legacy_transaction_listen_port,
      (ushort)tile->net.quic_transaction_listen_port,
      (ushort)tile->net.shred_listen_port,
      (ushort)tile->net.gossip_listen_port,
      (ushort)tile->net.repair_intake_listen_port,
      (ushort)tile->net.repair_serve_listen_port,
    };

    uint lo_idx = if_nametoindex( "lo" );
    if( FD_UNLIKELY( !lo_idx ) ) FD_LOG_ERR(( "if_nametoindex(lo) failed" ));

    fd_xdp_fds_t lo_fds = fd_xdp_install( lo_idx,
                                          tile->net.src_ip_addr,
                                          sizeof(udp_port_candidates)/sizeof(udp_port_candidates[0]),
                                          udp_port_candidates,
                                          "skb" );

    ctx->prog_link_fds[ 1 ] = lo_fds.prog_link_fd;
    ctx->xsk[ 1 ] =
        fd_xsk_join(
        fd_xsk_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_align(), fd_xsk_footprint( FD_NET_MTU, tile->net.xdp_rx_queue_size, tile->net.xdp_rx_queue_size, tile->net.xdp_tx_queue_size, tile->net.xdp_tx_queue_size ) ),
                    FD_NET_MTU,
                    tile->net.xdp_rx_queue_size,
                    tile->net.xdp_rx_queue_size,
                    tile->net.xdp_tx_queue_size,
                    tile->net.xdp_tx_queue_size ) );
    if( FD_UNLIKELY( !ctx->xsk[ 1 ] ) )                                                            FD_LOG_ERR(( "fd_xsk_join failed" ));
    if( FD_UNLIKELY( !fd_xsk_init( ctx->xsk[ 1 ], lo_idx, (uint)tile->kind_id, 0 /* flags */ ) ) ) FD_LOG_ERR(( "failed to bind lo_xsk" ));
    if( FD_UNLIKELY( !fd_xsk_activate( ctx->xsk[ 1 ], lo_fds.xsk_map_fd ) ) )                          FD_LOG_ERR(( "failed to activate lo_xsk" ));
    if( FD_UNLIKELY( -1==close( lo_fds.xsk_map_fd ) ) )                                                FD_LOG_ERR(( "close(%d) failed (%d-%s)", xsk_map_fd, errno, fd_io_strerror( errno ) ));

    ctx->xsk_aio[ 1 ] = fd_xsk_aio_join( fd_xsk_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_xsk_aio_align(), fd_xsk_aio_footprint( tile->net.xdp_tx_queue_size, tile->net.xdp_aio_depth ) ),
                                                         tile->net.xdp_tx_queue_size,
                                                         tile->net.xdp_aio_depth ), ctx->xsk[ 1 ] );
    if( FD_UNLIKELY( !ctx->xsk_aio[ 1 ] ) ) FD_LOG_ERR(( "fd_xsk_aio_new failed" ));
  }

  ctx->ip = fd_ip_join( fd_ip_new( FD_SCRATCH_ALLOC_APPEND( l, fd_ip_align(), fd_ip_footprint( 0UL, 0UL ) ), 0UL, 0UL ) );
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );

  fd_net_ctx_t *      ctx        = FD_SCRATCH_ALLOC_APPEND( l, alignof(fd_net_ctx_t),      sizeof(fd_net_ctx_t)      );
  fd_aio_t *          net_rx_aio = fd_aio_join( fd_aio_new( FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(), fd_aio_footprint() ), ctx, net_rx_aio_send ) );
  if( FD_UNLIKELY( !net_rx_aio ) ) FD_LOG_ERR(( "fd_aio_join failed" ));

  ctx->round_robin_cnt = fd_topo_tile_name_cnt( topo, tile->name );
  ctx->round_robin_id  = tile->kind_id;

  fd_xsk_aio_set_rx( ctx->xsk_aio[ 0 ], net_rx_aio );
  ctx->tx = fd_xsk_aio_get_tx( ctx->xsk_aio[ 0 ] );

  if( FD_UNLIKELY( ctx->xsk_cnt>1UL ) ) {
    fd_xsk_aio_set_rx( ctx->xsk_aio[ 1 ], net_rx_aio );
    ctx->lo_tx = fd_xsk_aio_get_tx( ctx->xsk_aio[ 1 ] );
  }

  ctx->src_ip_addr = tile->net.src_ip_addr;
  memcpy( ctx->src_mac_addr, tile->net.src_mac_addr, 6UL );

  ctx->metrics.tx_dropped_cnt = 0UL;

  ctx->shred_listen_port              = tile->net.shred_listen_port;
  ctx->quic_transaction_listen_port   = tile->net.quic_transaction_listen_port;
  ctx->legacy_transaction_listen_port = tile->net.legacy_transaction_listen_port;
  ctx->gossip_listen_port             = tile->net.gossip_listen_port;
  ctx->repair_intake_listen_port      = tile->net.repair_intake_listen_port;
  ctx->repair_serve_listen_port       = tile->net.repair_serve_listen_port;

  /* Put a bound on chunks we read from the input, to make sure they
     are within in the data region of the workspace. */

  if( FD_UNLIKELY( !tile->in_cnt ) ) FD_LOG_ERR(( "net tile in link cnt is zero" ));
  if( FD_UNLIKELY( tile->in_cnt>MAX_NET_INS ) ) FD_LOG_ERR(( "net tile in link cnt %lu exceeds MAX_NET_INS %lu", tile->in_cnt, MAX_NET_INS ));
  FD_TEST( tile->in_cnt<=32 );
  for( ulong i=0UL; i<tile->in_cnt; i++ ) {
    fd_topo_link_t * link = &topo->links[ tile->in_link_id[ i ] ];
    if( FD_UNLIKELY( link->mtu!=FD_NET_MTU ) ) FD_LOG_ERR(( "net tile in link does not have a normal MTU" ));

    ctx->in[ i ].mem    = topo->workspaces[ topo->objs[ link->dcache_obj_id ].wksp_id ].wksp;
    ctx->in[ i ].chunk0 = fd_dcache_compact_chunk0( ctx->in[ i ].mem, link->dcache );
    ctx->in[ i ].wmark  = fd_dcache_compact_wmark( ctx->in[ i ].mem, link->dcache, link->mtu );
  }

  for( ulong i = 0; i < tile->out_cnt; i++ ) {
    fd_topo_link_t * out_link = &topo->links[ tile->out_link_id[ i  ] ];
    if( strcmp( out_link->name, "net_quic" ) == 0 ) {
      fd_topo_link_t * quic_out = out_link;
      ctx->quic_out->mcache = quic_out->mcache;
      ctx->quic_out->sync   = fd_mcache_seq_laddr( ctx->quic_out->mcache );
      ctx->quic_out->depth  = fd_mcache_depth( ctx->quic_out->mcache );
      ctx->quic_out->seq    = fd_mcache_seq_query( ctx->quic_out->sync );
      ctx->quic_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( quic_out->dcache ), quic_out->dcache );
      ctx->quic_out->mem    = topo->workspaces[ topo->objs[ quic_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->quic_out->wmark  = fd_dcache_compact_wmark ( ctx->quic_out->mem, quic_out->dcache, quic_out->mtu );
      ctx->quic_out->chunk  = ctx->quic_out->chunk0;
    } else if( strcmp( out_link->name, "net_shred" ) == 0 ) {
      fd_topo_link_t * shred_out = out_link;
      ctx->shred_out->mcache = shred_out->mcache;
      ctx->shred_out->sync   = fd_mcache_seq_laddr( ctx->shred_out->mcache );
      ctx->shred_out->depth  = fd_mcache_depth( ctx->shred_out->mcache );
      ctx->shred_out->seq    = fd_mcache_seq_query( ctx->shred_out->sync );
      ctx->shred_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( shred_out->dcache ), shred_out->dcache );
      ctx->shred_out->mem    = topo->workspaces[ topo->objs[ shred_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->shred_out->wmark  = fd_dcache_compact_wmark ( ctx->shred_out->mem, shred_out->dcache, shred_out->mtu );
      ctx->shred_out->chunk  = ctx->shred_out->chunk0;
    } else if( strcmp( out_link->name, "net_gossip" ) == 0 ) {
      fd_topo_link_t * gossip_out = out_link;
      ctx->gossip_out->mcache = gossip_out->mcache;
      ctx->gossip_out->sync   = fd_mcache_seq_laddr( ctx->gossip_out->mcache );
      ctx->gossip_out->depth  = fd_mcache_depth( ctx->gossip_out->mcache );
      ctx->gossip_out->seq    = fd_mcache_seq_query( ctx->gossip_out->sync );
      ctx->gossip_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( gossip_out->dcache ), gossip_out->dcache );
      ctx->gossip_out->mem    = topo->workspaces[ topo->objs[ gossip_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->gossip_out->wmark  = fd_dcache_compact_wmark ( ctx->gossip_out->mem, gossip_out->dcache, gossip_out->mtu );
      ctx->gossip_out->chunk  = ctx->gossip_out->chunk0;
    } else if( strcmp( out_link->name, "net_repair" ) == 0 ) {
      fd_topo_link_t * repair_out = out_link;
      ctx->repair_out->mcache = repair_out->mcache;
      ctx->repair_out->sync   = fd_mcache_seq_laddr( ctx->repair_out->mcache );
      ctx->repair_out->depth  = fd_mcache_depth( ctx->repair_out->mcache );
      ctx->repair_out->seq    = fd_mcache_seq_query( ctx->repair_out->sync );
      ctx->repair_out->chunk0 = fd_dcache_compact_chunk0( fd_wksp_containing( repair_out->dcache ), repair_out->dcache );
      ctx->repair_out->mem    = topo->workspaces[ topo->objs[ repair_out->dcache_obj_id ].wksp_id ].wksp;
      ctx->repair_out->wmark  = fd_dcache_compact_wmark ( ctx->repair_out->mem, repair_out->dcache, repair_out->mtu );
      ctx->repair_out->chunk  = ctx->repair_out->chunk0;
    } else {
      FD_LOG_ERR(( "unrecognized out link `%s`", out_link->name ));
    }
  }

  /* Check if any of the tiles we set a listen port for do not have an outlink. */
  if( FD_UNLIKELY( ctx->shred_listen_port!=0 && ctx->shred_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "shred listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->quic_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "quic transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->legacy_transaction_listen_port!=0 && ctx->quic_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "legacy transaction listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->gossip_listen_port!=0 && ctx->gossip_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "gossip listen port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_intake_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair intake port set but no out link was found" ));
  } else if( FD_UNLIKELY( ctx->repair_serve_listen_port!=0 && ctx->repair_out->mcache==NULL ) ) {
    FD_LOG_ERR(( "repair serve listen port set but no out link was found" ));
  }

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  if( FD_UNLIKELY( scratch_top > (ulong)scratch + scratch_footprint( tile ) ) )
    FD_LOG_ERR(( "scratch overflow %lu %lu %lu", scratch_top - (ulong)scratch - scratch_footprint( tile ), scratch_top, (ulong)scratch + scratch_footprint( tile ) ));
}

static ulong
populate_allowed_seccomp( fd_topo_t const *      topo,
                          fd_topo_tile_t const * tile,
                          ulong                  out_cnt,
                          struct sock_filter *   out ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ), sizeof( fd_net_ctx_t ) );

  /* A bit of a hack, if there is no loopback XSK for this tile, we still need to pass
     two "allow" FD arguments to the net policy, so we just make them both the same. */
  int allow_fd2 = ctx->xsk_cnt>1UL ? ctx->xsk[ 1 ]->xsk_fd : ctx->xsk[ 0 ]->xsk_fd;
  FD_TEST( ctx->xsk[ 0 ]->xsk_fd >= 0 && allow_fd2 >= 0 );
  int netlink_fd = fd_ip_netlink_get( ctx->ip )->fd;
  populate_sock_filter_policy_net( out_cnt, out, (uint)fd_log_private_logfile_fd(), (uint)ctx->xsk[ 0 ]->xsk_fd, (uint)allow_fd2, (uint)netlink_fd );
  return sock_filter_policy_net_instr_cnt;
}

static ulong
populate_allowed_fds( fd_topo_t const *      topo,
                      fd_topo_tile_t const * tile,
                      ulong                  out_fds_cnt,
                      int *                  out_fds ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );
  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_net_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_net_ctx_t ), sizeof( fd_net_ctx_t ) );

  if( FD_UNLIKELY( out_fds_cnt<7UL ) ) FD_LOG_ERR(( "out_fds_cnt %lu", out_fds_cnt ));

  ulong out_cnt = 0UL;

  out_fds[ out_cnt++ ] = 2; /* stderr */
  if( FD_LIKELY( -1!=fd_log_private_logfile_fd() ) )
    out_fds[ out_cnt++ ] = fd_log_private_logfile_fd(); /* logfile */
  out_fds[ out_cnt++ ] = fd_ip_netlink_get( ctx->ip )->fd;

                                      out_fds[ out_cnt++ ] = ctx->xsk[ 0 ]->xsk_fd;
                                      out_fds[ out_cnt++ ] = ctx->prog_link_fds[ 0 ];
  if( FD_LIKELY( ctx->xsk_cnt>1UL ) ) out_fds[ out_cnt++ ] = ctx->xsk[ 1 ]->xsk_fd;
  if( FD_LIKELY( ctx->xsk_cnt>1UL ) ) out_fds[ out_cnt++ ] = ctx->prog_link_fds[ 1 ];
  return out_cnt;
}

#define STEM_BURST (1UL)

#define STEM_CALLBACK_CONTEXT_TYPE  fd_net_ctx_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_net_ctx_t)

#define STEM_CALLBACK_METRICS_WRITE       metrics_write
#define STEM_CALLBACK_DURING_HOUSEKEEPING during_housekeeping
#define STEM_CALLBACK_BEFORE_CREDIT       before_credit
#define STEM_CALLBACK_BEFORE_FRAG         before_frag
#define STEM_CALLBACK_DURING_FRAG         during_frag
#define STEM_CALLBACK_AFTER_FRAG          after_frag

#include "../../../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_net = {
  .name                     = "net",
  .populate_allowed_seccomp = populate_allowed_seccomp,
  .populate_allowed_fds     = populate_allowed_fds,
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = privileged_init,
  .unprivileged_init        = unprivileged_init,
  .run                      = stem_run,
};
