/*
 *
 * (C) 2021-22 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "include.h"

/* Forward */
int netfilter_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
		       struct nfq_data *nfa, void *data);

/* **************************************************** */

NwInterface::NwInterface(u_int nf_device_id,
				       Configuration *_c,
				       GeoIP *_g) {
  conf = _c, geoip = _g;

  queueId = nf_device_id, nfHandle = nfq_open();

  if(nfHandle == NULL) {
    trace->traceEvent(TRACE_ERROR, "Unable to get netfilter handle [queueId=%d]", queueId);
    throw 1;
  }

  if(nfq_unbind_pf(nfHandle, AF_INET) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to unbind [queueId=%d]: are you root ?", queueId);
    throw 1;
  }

  if(nfq_bind_pf(nfHandle, AF_INET) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to bind [queueId=%d]", queueId);
    throw 1;
  }

  if((queueHandle = nfq_create_queue(nfHandle, queueId, &netfilter_callback, this)) == NULL) {
    trace->traceEvent(TRACE_ERROR, "Unable to attach to NF_QUEUE %d: is it already in use?", queueId);
    throw 1;
  } else
    trace->traceEvent(TRACE_NORMAL, "Succesfully connected to NF_QUEUE %d", queueId);

#if !defined(__mips__)
  nfnl_rcvbufsiz(nfq_nfnlh(nfHandle), NF_BUFFER_SIZE);
#endif

  if(nfq_set_mode(queueHandle, NFQNL_COPY_PACKET, 0XFFFF) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to set packet_copy mode");
    throw 1;
  }

  if(nfq_set_queue_maxlen(queueHandle, NF_MAX_QUEUE_LEN) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to set queue len");
    throw 1;
  }

  nf_fd = nfq_fd(nfHandle);
}

/* **************************************************** */

NwInterface::~NwInterface() {
  if(queueHandle) nfq_destroy_queue(queueHandle);
  if(nfHandle)    nfq_close(nfHandle);

  nf_fd = 0;
}

/* **************************************************** */

int netfilter_callback(struct nfq_q_handle *qh,
		       struct nfgenmsg *nfmsg,
		       struct nfq_data *nfa,
		       void *data) {
  const u_char *payload;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
  NwInterface *iface = (NwInterface *)data;
  u_int payload_len;
  u_int32_t id = ntohl(ph->packet_id);
  u_int16_t marker;

  if(!ph) return(-1);

#ifdef HAVE_NFQ_SET_VERDICT2
  payload_len = nfq_get_payload(nfa, (unsigned char **)&payload);
#else
  payload_len = nfq_get_payload(nfa, (char **)&payload);
#endif

  marker = iface->dissectPacket(payload, payload_len);

  return(nfq_set_verdict2(qh, id, NF_ACCEPT, marker, 0, NULL));
}

/* **************************************************** */

void NwInterface::packetPollLoop() {
  struct nfq_handle *h;
  int fd;

  ifaceRunning = true;

  h = get_nfHandle();
  fd = get_fd();

  while(isRunning()) {
    fd_set mask;
    struct timeval wait_time;

    FD_ZERO(&mask);
    FD_SET(fd, &mask);
    wait_time.tv_sec = 1, wait_time.tv_usec = 0;

    if(select(fd+1, &mask, 0, 0, &wait_time) > 0) {
      char pktBuf[8192] __attribute__ ((aligned));
      int len = recv(fd, pktBuf, sizeof(pktBuf), 0);

      // trace->traceEvent(TRACE_INFO, "Pkt len %d", len);

      if(len >= 0) {
	int rc = nfq_handle_packet(h, pktBuf, len);

	if(rc < 0)
	  trace->traceEvent(TRACE_ERROR, "nfq_handle_packet() failed: [len: %d][rc: %d][errno: %d]", len, rc, errno);
      } else {
	trace->traceEvent(TRACE_ERROR, "NF_QUEUE receive error: [len: %d][errno: %d]", len, errno);
	break;
      }
    }
  }

  trace->traceEvent(TRACE_NORMAL, "Leaving netfilter packet poll loop");

  ifaceRunning = false;
}

/* **************************************************** */

Marker NwInterface::dissectPacket(const u_char *payload, u_int payload_len) {
  /* We can see only IP addresses */
  u_int16_t ip_offset = 0, vlan_id = 0 /* FIX */;

  switch((payload[ip_offset] & 0xf0) >> 4) {
  case 4:
    /* OK */
    break;
  default:
    return(MARKER_PASS); /* Pass */
  }

  if(payload_len >= ip_offset) {
    struct iphdr *iph = (struct iphdr *) &payload[ip_offset];

    if(iph->version != 4) {
      /* This is not IPv4 */
      return(MARKER_PASS); /* TODO */
    } else {
      u_int8_t *l4;
      struct tcphdr *tcph;
      struct udphdr *udph;
      u_int16_t src_port, dst_port;
      u_int8_t l4_proto, frag_off = ntohs(iph->frag_off);

      if((iph->protocol == IPPROTO_UDP) && ((frag_off & 0x3FFF /* IP_MF | IP_OFFSET */ ) != 0))
	return(MARKER_UNKNOWN); /* Don't block it */

      l4_proto = iph->protocol;
      l4 = ((u_int8_t *) iph + iph->ihl * 4);

      switch(l4_proto) {
      case IPPROTO_TCP:
	tcph = (struct tcphdr *)l4;
	src_port = tcph->source, dst_port = tcph->dest;
	break;

      case IPPROTO_UDP:
	udph = (struct udphdr *)l4;
	src_port = udph->source, dst_port = udph->dest;
	break;

      default:
	src_port = dst_port = 0;
	break;
      }

      return(makeVerdict(l4_proto, vlan_id,
			 iph->saddr, src_port,
			 iph->daddr, dst_port));
    }
  }

  return(MARKER_PASS);
}

/* **************************************************** */

const char* NwInterface::getProtoName(u_int8_t proto) {
  switch(proto) {
  case IPPROTO_TCP:  return("TCP");
  case IPPROTO_UDP:  return("UDP");
  case IPPROTO_ICMP: return("ICMP");
  default:           return("???");
  }
}

/* **************************************************** */

bool NwInterface::isPrivateIPv4(u_int32_t addr /* network byte order */) {
  u_int32_t a = ntohl(addr);

  if(((a & 0xFF000000) == 0x0A000000 /* 10.0.0.0/8 */)
     || ((a & 0xFFF00000) == 0xAC100000 /* 172.16.0.0/12 */)
     || ((a & 0xFFFF0000) == 0xC0A80000 /* 192.168.0.0/16 */)
     || ((a & 0xFF000000) == 0x7F000000 /* 127.0.0.0/8 */)
     || ((a & 0xFFFF0000) == 0xA9FE0000 /* 169.254.0.0/16 Link-Local communication rfc3927 */)
     || (a == 0xFFFFFFFF /* 255.255.255.255 */)
     || (a == 0x0        /* 0.0.0.0 */)
     || ((a & 0xF0000000) == 0xE0000000 /* 224.0.0.0/4 */))
    return(true);
  else
    return(false);
}

/* **************************************************** */

void NwInterface::logFlow(const char *proto_name,
			  char *src_host, u_int16_t sport, char *src_country, char *src_continent, bool src_blacklisted,
			  char *dst_host, u_int16_t dport, char *dst_country, char *dst_continent, bool dst_blacklisted,
			  bool pass_verdict) {
  Json::Value root;
  std::string json_txt;
  Json::FastWriter writer;
  
  root["proto"] = proto_name;

  root["src"]["host"] = src_host;
  root["src"]["port"] = sport;
  if(src_country && (src_country[0] != '\0')) root["src"]["country"] = src_country;
  if(src_continent && (src_continent[0] != '\0')) root["src"]["continent"] = src_continent;
  if(src_blacklisted) root["src"]["blacklisted"] = src_blacklisted;

  root["dst"]["host"] = dst_host;
  root["dst"]["port"] = dport;
  if(dst_country && (dst_country[0] != '\0')) root["dst"]["country"] = dst_country;
  if(dst_continent && (dst_continent[0] != '\0')) root["dst"]["continent"] = dst_continent;
  if(dst_blacklisted) root["dst"]["blacklisted"] = dst_blacklisted;
  
  root["verdict"] = pass_verdict ? "pass" : "drop";

  json_txt = writer.write(root);

  if(pass_verdict)
    trace->traceEvent(TRACE_INFO, "%s", json_txt.c_str());
  else
    trace->traceEvent(TRACE_WARNING, "%s", json_txt.c_str());
}

/* **************************************************** */

Marker NwInterface::makeVerdict(u_int8_t proto, u_int16_t vlanId,
				u_int32_t saddr /* network byte order */,
				u_int16_t sport /* network byte order */,
				u_int32_t daddr /* network byte order */,
				u_int16_t dport /* network byte order */) {
  struct in_addr in;
  char *host, src_host[32], dst_host[32], src_country[3]={'\0'}, dst_country[3]={'\0'},
   src_cont[3]={'\0'}, dst_cont[3]={'\0'} ;
  const char *proto_name = getProtoName(proto);
  bool pass_local = true, saddr_private = isPrivateIPv4(saddr), daddr_private = isPrivateIPv4(daddr);;
  Marker m, src_marker, dst_marker;
  struct in_addr addr;

  in.s_addr = saddr;
  host = inet_ntoa(in);
  strncpy(src_host, host, sizeof(src_host)-1);

  in.s_addr = daddr;
  host = inet_ntoa(in);
  strncpy(dst_host, host, sizeof(dst_host)-1);

  /* Step 1 - For all ports/protocols, check if sender/recipient are blacklisted and if so, block this flow */
  addr.s_addr = saddr;
  if((!saddr_private) && conf->isBlacklistedIPv4(&addr)) {
    logFlow(proto_name,
	    src_host, sport, src_country, src_cont, true,
	    dst_host, dport, dst_country, dst_cont, false,
	    false /* drop */);

    return(MARKER_DROP);
  }

  addr.s_addr = daddr;
  if((!daddr_private) && conf->isBlacklistedIPv4(&addr)) {
    logFlow(proto_name,
	    src_host, sport, src_country, src_cont, false,
	    dst_host, dport, dst_country, dst_cont, true,
	    false /* drop */);

    return(MARKER_DROP);
  }

  sport = ntohs(sport), dport = ntohs(dport);

  /* Step 2 - For TCP/UDP ignore traffic for non-monitored ports */
  switch(proto) {
  case IPPROTO_TCP:
    if((conf->isMonitoredTCPPort(sport)) || conf->isMonitoredTCPPort(dport))
      ;
    else {
      trace->traceEvent(TRACE_INFO, "Ignoring TCP ports %u/%u", sport, dport);
      return(MARKER_PASS);
    }
    break;

  case IPPROTO_UDP:
    if((!conf->isMonitoredUDPPort(sport)) || conf->isMonitoredUDPPort(dport))
      ;
    else {
      trace->traceEvent(TRACE_INFO, "Ignoring UDP ports %u/%u", sport, dport);
      return(MARKER_PASS);
    }
    break;
  }

  src_marker = dst_marker = conf->getDefaultPolicy();

  /* Step 3 - For monitored TCP/UDP ports (and ICMP) check the country blacklist */
  in.s_addr = saddr;
  host = inet_ntoa(in);
  strncpy(src_host, host, sizeof(src_host)-1);

  if((!saddr_private) && (geoip->lookup(host, src_country, sizeof(src_country), src_cont, sizeof(src_cont)))) {
    src_marker = conf->getMarker(src_country, src_cont);
    pass_local = false;
  } else {
    /* Unknown or private IP address  */

    src_marker = MARKER_PASS;
  }

  in.s_addr = daddr;
  host = inet_ntoa(in);
  strncpy(dst_host, host, sizeof(dst_host)-1);

  if((!daddr_private) && (geoip->lookup(host = inet_ntoa(in), dst_country, sizeof(dst_country), dst_cont, sizeof(dst_cont)))) {
    dst_marker = conf->getMarker(dst_country, dst_cont);
    pass_local = false;
  } else {
    /* Unknown or private IP address  */
    dst_marker = MARKER_PASS;
  }

  /* Final step: compute the flow verdict */
  if((conf->isIgnoredPort(sport) || conf->isIgnoredPort(dport))
     || ((src_marker == MARKER_PASS) && (dst_marker == MARKER_PASS))) {
    m = MARKER_PASS;

    logFlow(proto_name,
	    src_host, sport, src_country, src_cont, false,
	    dst_host, dport, dst_country, dst_cont, false,
	    true /* pass */);
  } else {
    m = MARKER_DROP;

    logFlow(proto_name,
	    src_host, sport, src_country, src_cont, false,
	    dst_host, dport, dst_country, dst_cont, false,
	    false /* drop */);
  }

  return(m);
}
