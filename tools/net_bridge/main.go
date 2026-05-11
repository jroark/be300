package main

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

var (
	gatewayMAC = []byte{0x60, 0x50, 0x40, 0x30, 0x20, 0x10}
	bcastMAC   = []byte{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}
	gatewayIP  = ip4("10.0.0.254")
	guestIP    = ip4("10.0.0.1")
	zeroIP     = ip4("0.0.0.0")
	bcastIP    = ip4("255.255.255.255")
)

const (
	bridgeVersion = "20260511a"
	ethIPv4       = 0x0800
	ethARP        = 0x0806
	tcpMSS        = 1200
	udpIdle       = 60 * time.Second
	tcpIdle       = 120 * time.Second
	maxWSFrame    = 4096
)

type bridge struct {
	mu      sync.Mutex
	flowsMu sync.Mutex
	client  net.Conn
	udp     map[string]*udpFlow
	tcp     map[string]*tcpFlow
	ipID    uint16
	nextSeq uint32
}

type udpFlow struct {
	conn      *net.UDPConn
	guestMAC  []byte
	guestIP   []byte
	guestPort uint16
	timer     *time.Timer
	mu        sync.Mutex
}

type tcpFlow struct {
	key        string
	conn       net.Conn
	guestMAC   []byte
	guestIP    []byte
	guestPort  uint16
	remoteIP   []byte
	remotePort uint16
	clientNext uint32
	serverNext uint32
	pending    [][]byte
	connected  bool
	lastActive time.Time
	timer      *time.Ticker
	mu         sync.Mutex
}

type ipv4Packet struct {
	srcMAC  []byte
	dstMAC  []byte
	srcIP   []byte
	dstIP   []byte
	proto   byte
	payload []byte
}

func main() {
	host := flag.String("host", "127.0.0.1", "listen address")
	port := flag.Int("port", 8765, "listen port")
	flag.Parse()

	b := &bridge{
		udp:     make(map[string]*udpFlow),
		tcp:     make(map[string]*tcpFlow),
		ipID:    1,
		nextSeq: 0x41000000,
	}

	addr := fmt.Sprintf("%s:%d", *host, *port)
	server := &http.Server{Addr: addr, Handler: http.HandlerFunc(b.serve)}
	fmt.Printf("[bridge] BE-300 network bridge %s\n", bridgeVersion)
	fmt.Printf("[bridge] listening on ws://%s\n", addr)
	fmt.Println("[bridge] set Bridge URL in the web app to this address")
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}

func (b *bridge) serve(w http.ResponseWriter, r *http.Request) {
	if !strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		w.Header().Set("content-type", "text/plain")
		_, _ = w.Write([]byte("BE-300 network bridge is running.\n"))
		return
	}
	key := r.Header.Get("Sec-WebSocket-Key")
	hijacker, ok := w.(http.Hijacker)
	if !ok || key == "" {
		http.Error(w, "websocket upgrade unavailable", http.StatusBadRequest)
		return
	}
	conn, rw, err := hijacker.Hijack()
	if err != nil {
		return
	}
	accept := wsAccept(key)
	_, _ = rw.WriteString("HTTP/1.1 101 Switching Protocols\r\n")
	_, _ = rw.WriteString("Upgrade: websocket\r\n")
	_, _ = rw.WriteString("Connection: Upgrade\r\n")
	_, _ = rw.WriteString("Sec-WebSocket-Accept: " + accept + "\r\n\r\n")
	_ = rw.Flush()

	b.mu.Lock()
	if b.client != nil && b.client != conn {
		_ = b.client.Close()
	}
	b.client = conn
	b.mu.Unlock()
	fmt.Println("[bridge] browser connected")
	b.readWebSocket(conn, rw.Reader)
}

func wsAccept(key string) string {
	sum := sha1.Sum([]byte(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	return base64.StdEncoding.EncodeToString(sum[:])
}

func (b *bridge) readWebSocket(conn net.Conn, r *bufio.Reader) {
	defer func() {
		b.mu.Lock()
		if b.client == conn {
			b.client = nil
		}
		b.mu.Unlock()
		_ = conn.Close()
		fmt.Println("[bridge] browser disconnected")
	}()

	for {
		opcode, payload, err := wsReadFrame(r)
		if err != nil {
			return
		}
		switch opcode {
		case 0x2:
			b.handleEthernet(payload)
		case 0x8:
			return
		case 0x9:
			_ = wsWriteFrame(conn, 0xA, payload)
		}
	}
}

func wsReadFrame(r *bufio.Reader) (byte, []byte, error) {
	h, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	b, err := r.ReadByte()
	if err != nil {
		return 0, nil, err
	}
	opcode := h & 0x0f
	masked := b&0x80 != 0
	length := int(b & 0x7f)
	if length == 126 {
		var ext [2]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		length = int(binary.BigEndian.Uint16(ext[:]))
	} else if length == 127 {
		var ext [8]byte
		if _, err := io.ReadFull(r, ext[:]); err != nil {
			return 0, nil, err
		}
		n := binary.BigEndian.Uint64(ext[:])
		if n > maxWSFrame {
			return 0, nil, fmt.Errorf("websocket frame too large")
		}
		length = int(n)
	}
	if length > maxWSFrame {
		return 0, nil, fmt.Errorf("websocket frame too large")
	}
	var mask [4]byte
	if masked {
		if _, err := io.ReadFull(r, mask[:]); err != nil {
			return 0, nil, err
		}
	}
	payload := make([]byte, length)
	if _, err := io.ReadFull(r, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i&3]
		}
	}
	return opcode, payload, nil
}

func wsWriteFrame(w io.Writer, opcode byte, payload []byte) error {
	header := []byte{0x80 | opcode, 0}
	if len(payload) < 126 {
		header[1] = byte(len(payload))
	} else {
		header[1] = 126
		var ext [2]byte
		binary.BigEndian.PutUint16(ext[:], uint16(len(payload)))
		header = append(header, ext[:]...)
	}
	if _, err := w.Write(header); err != nil {
		return err
	}
	_, err := w.Write(payload)
	return err
}

func (b *bridge) sendEthernet(frame []byte) {
	b.mu.Lock()
	c := b.client
	b.mu.Unlock()
	if c != nil {
		_ = wsWriteFrame(c, 0x2, frame)
	}
}

func (b *bridge) handleEthernet(frame []byte) {
	if len(frame) < 14 {
		return
	}
	typ := be16(frame[12:14])
	switch typ {
	case ethARP:
		b.handleARP(frame)
	case ethIPv4:
		if ip := parseIPv4(frame); ip != nil {
			switch ip.proto {
			case 1:
				b.handleICMP(ip)
			case 6:
				b.handleTCP(ip)
			case 17:
				b.handleUDP(ip)
			}
		}
	}
}

func (b *bridge) handleARP(frame []byte) {
	if len(frame) < 42 || be16(frame[20:22]) != 1 {
		return
	}
	targetIP := frame[38:42]
	if eq(targetIP, guestIP) {
		return
	}
	payload := make([]byte, 28)
	put16(payload[0:2], 1)
	put16(payload[2:4], ethIPv4)
	payload[4], payload[5] = 6, 4
	put16(payload[6:8], 2)
	copy(payload[8:14], gatewayMAC)
	copy(payload[14:18], targetIP)
	copy(payload[18:24], frame[6:12])
	copy(payload[24:28], frame[28:32])
	b.sendEthernet(ethernet(frame[6:12], gatewayMAC, ethARP, payload))
	fmt.Printf("[bridge] ARP %s is at %s\n", ipText(targetIP), macText(gatewayMAC))
}

func (b *bridge) handleUDP(ip *ipv4Packet) {
	if len(ip.payload) < 8 {
		return
	}
	srcPort := be16(ip.payload[0:2])
	dstPort := be16(ip.payload[2:4])
	udpLen := int(be16(ip.payload[4:6]))
	if udpLen < 8 || udpLen > len(ip.payload) {
		return
	}
	body := ip.payload[8:udpLen]
	if srcPort == 68 && dstPort == 67 {
		b.handleDHCP(ip, body)
		return
	}
	if dstPort == 53 {
		b.handleDNS(ip, srcPort, body)
		return
	}
	b.proxyUDP(ip, srcPort, dstPort, body)
}

func (b *bridge) handleDHCP(ip *ipv4Packet, req []byte) {
	if len(req) < 240 || !eq(req[236:240], []byte{99, 130, 83, 99}) {
		return
	}
	replyType := byte(2)
	if dhcpType(req) == 3 {
		replyType = 5
	}
	payload := make([]byte, 300)
	copy(payload, req[:min(len(req), 240)])
	payload[0] = 2
	copy(payload[16:20], guestIP)
	copy(payload[20:24], gatewayIP)
	copy(payload[108:], []byte("be300"))
	copy(payload[236:240], []byte{99, 130, 83, 99})
	off := 240
	off = dhcpOpt(payload, off, 53, []byte{replyType})
	off = dhcpOpt(payload, off, 1, []byte{255, 255, 255, 0})
	off = dhcpOpt(payload, off, 3, gatewayIP)
	off = dhcpOpt(payload, off, 6, gatewayIP)
	off = dhcpOpt(payload, off, 54, gatewayIP)
	off = dhcpOpt(payload, off, 51, []byte{0, 1, 0x51, 0x80})
	off = dhcpOpt(payload, off, 58, []byte{0, 0, 0xa8, 0xc0})
	off = dhcpOpt(payload, off, 59, []byte{0, 0, 0xd2, 0xf0})
	payload[off] = 255
	body := payload[:off+1]
	udp := udpPacket(gatewayIP, bcastIP, 67, 68, body)
	b.sendEthernet(ethernet(dhcpReplyMAC(ip, req), gatewayMAC, ethIPv4, udp))
	fmt.Printf("[bridge] DHCP %s %s to %s\n", map[byte]string{2: "OFFER", 5: "ACK"}[replyType], ipText(guestIP), macText(ip.srcMAC))
}

func (b *bridge) handleDNS(ip *ipv4Packet, srcPort uint16, query []byte) {
	q := dnsQuestion(query)
	var answers []net.IP
	rcode := byte(0)

	if q.name != "" && q.qclass == 1 {
		switch q.qtype {
		case 1, 255:
			if ips, err := net.LookupIP(q.name); err == nil {
				for _, candidate := range ips {
					if v4 := candidate.To4(); v4 != nil {
						answers = append(answers, v4)
						if len(answers) == 4 {
							break
						}
					}
				}
			} else if dnsNameNotFound(err) {
				rcode = 3
			} else {
				rcode = 2
			}
		case 28:
			rcode = 0
		default:
			rcode = 0
		}
	}
	response := dnsResponse(query, answers, rcode)
	if len(response) == 0 {
		return
	}
	serverIP := ip.dstIP
	if eq(serverIP, zeroIP) || eq(serverIP, bcastIP) {
		serverIP = gatewayIP
	}
	udp := udpPacket(serverIP, ip.srcIP, 53, srcPort, response)
	b.sendEthernet(ethernet(ip.srcMAC, gatewayMAC, ethIPv4, udp))
	if q.name != "" {
		fmt.Printf("[bridge] DNS %s %s via %s -> %d A record(s), rcode %d\n",
			q.name, dnsTypeText(q.qtype), ipText(serverIP), len(answers), rcode)
	}
}

func dnsNameNotFound(err error) bool {
	var dnsErr *net.DNSError
	return errors.As(err, &dnsErr) && dnsErr.IsNotFound
}

func dnsTypeText(qtype uint16) string {
	switch qtype {
	case 1:
		return "A"
	case 28:
		return "AAAA"
	case 255:
		return "ANY"
	default:
		return fmt.Sprintf("TYPE%d", qtype)
	}
}

func dhcpReplyMAC(ip *ipv4Packet, req []byte) []byte {
	if dhcpBroadcast(req) || eq(ip.dstMAC, bcastMAC) || eq(ip.srcIP, zeroIP) {
		return bcastMAC
	}
	return ip.srcMAC
}

func dhcpBroadcast(payload []byte) bool {
	return len(payload) >= 12 && be16(payload[10:12])&0x8000 != 0
}

func (b *bridge) proxyUDP(ip *ipv4Packet, srcPort, dstPort uint16, body []byte) {
	key := fmt.Sprintf("%s:%d>%s:%d", ipText(ip.srcIP), srcPort, ipText(ip.dstIP), dstPort)
	b.flowsMu.Lock()
	flow := b.udp[key]
	b.flowsMu.Unlock()
	if flow == nil {
		raddr, err := net.ResolveUDPAddr("udp4", fmt.Sprintf("%s:%d", ipText(ip.dstIP), dstPort))
		if err != nil {
			fmt.Printf("[bridge] UDP resolve failed %s:%d: %v\n", ipText(ip.dstIP), dstPort, err)
			return
		}
		conn, err := net.DialUDP("udp4", nil, raddr)
		if err != nil {
			fmt.Printf("[bridge] UDP dial failed %s:%d: %v\n", ipText(ip.dstIP), dstPort, err)
			return
		}
		newFlow := &udpFlow{conn: conn, guestMAC: clone(ip.srcMAC), guestIP: clone(ip.srcIP), guestPort: srcPort}
		b.flowsMu.Lock()
		flow = b.udp[key]
		if flow == nil {
			flow = newFlow
			b.udp[key] = flow
			go b.readUDP(key, flow)
		} else {
			_ = conn.Close()
		}
		b.flowsMu.Unlock()
	}
	flow.mu.Lock()
	if flow.timer != nil {
		flow.timer.Stop()
	}
	flow.timer = time.AfterFunc(udpIdle, func() { b.closeUDP(key) })
	flow.mu.Unlock()
	_, _ = flow.conn.Write(body)
}

func (b *bridge) readUDP(key string, flow *udpFlow) {
	defer b.closeUDP(key)
	buf := make([]byte, 2048)
	for {
		n, addr, err := flow.conn.ReadFromUDP(buf)
		if err != nil {
			return
		}
		src := addr.IP.To4()
		if src == nil {
			continue
		}
		udp := udpPacket(src, flow.guestIP, uint16(addr.Port), flow.guestPort, buf[:n])
		b.sendEthernet(ethernet(flow.guestMAC, gatewayMAC, ethIPv4, udp))
	}
}

func (b *bridge) closeUDP(key string) {
	b.flowsMu.Lock()
	flow := b.udp[key]
	if flow != nil {
		delete(b.udp, key)
	}
	b.flowsMu.Unlock()
	if flow != nil {
		flow.mu.Lock()
		if flow.timer != nil {
			flow.timer.Stop()
			flow.timer = nil
		}
		flow.mu.Unlock()
		_ = flow.conn.Close()
	}
}

func (b *bridge) handleICMP(ip *ipv4Packet) {
	if len(ip.payload) < 8 || ip.payload[0] != 8 || !eq(ip.dstIP, gatewayIP) {
		return
	}
	reply := clone(ip.payload)
	reply[0], reply[2], reply[3] = 0, 0, 0
	put16(reply[2:4], checksum(reply))
	b.sendIP(ip.srcMAC, gatewayIP, ip.srcIP, 1, reply)
}

func (b *bridge) handleTCP(ip *ipv4Packet) {
	if len(ip.payload) < 20 {
		return
	}
	srcPort := be16(ip.payload[0:2])
	dstPort := be16(ip.payload[2:4])
	seq := be32(ip.payload[4:8])
	off := int(ip.payload[12]>>4) * 4
	flags := ip.payload[13]
	if off < 20 || off > len(ip.payload) {
		return
	}
	data := ip.payload[off:]
	key := fmt.Sprintf("%s:%d>%s:%d", ipText(ip.srcIP), srcPort, ipText(ip.dstIP), dstPort)
	flow := b.getTCP(key)
	if flags&0x02 != 0 {
		if flow == nil {
			flow = b.openTCP(key, ip, srcPort, dstPort, seq)
		}
		b.sendTCP(flow, 0x12, nil)
		flow.mu.Lock()
		flow.serverNext++
		flow.mu.Unlock()
		return
	}
	if flow == nil {
		return
	}
	flow.mu.Lock()
	flow.lastActive = time.Now()
	flow.mu.Unlock()
	if len(data) > 0 {
		var conn net.Conn
		flow.mu.Lock()
		flow.clientNext = seq + uint32(len(data))
		if flow.connected && flow.conn != nil {
			conn = flow.conn
		} else {
			flow.pending = append(flow.pending, clone(data))
		}
		flow.mu.Unlock()
		if conn != nil {
			_, _ = conn.Write(data)
		}
		b.sendTCP(flow, 0x10, nil)
	}
	if flags&0x01 != 0 {
		flow.mu.Lock()
		flow.clientNext = seq + uint32(len(data)) + 1
		conn := flow.conn
		flow.mu.Unlock()
		b.sendTCP(flow, 0x10, nil)
		if conn != nil {
			_ = conn.Close()
		}
	}
	if flags&0x04 != 0 {
		b.closeTCP(key, true)
	}
}

func (b *bridge) getTCP(key string) *tcpFlow {
	b.flowsMu.Lock()
	defer b.flowsMu.Unlock()
	return b.tcp[key]
}

func (b *bridge) openTCP(key string, ip *ipv4Packet, srcPort, dstPort uint16, seq uint32) *tcpFlow {
	b.flowsMu.Lock()
	flow := &tcpFlow{
		key:        key,
		guestMAC:   clone(ip.srcMAC),
		guestIP:    clone(ip.srcIP),
		guestPort:  srcPort,
		remoteIP:   clone(ip.dstIP),
		remotePort: dstPort,
		clientNext: seq + 1,
		serverNext: b.nextSeq,
		lastActive: time.Now(),
	}
	b.nextSeq += 0x10000
	b.tcp[key] = flow
	b.flowsMu.Unlock()
	fmt.Printf("[bridge] TCP %s:%d -> %s:%d\n", ipText(ip.srcIP), srcPort, ipText(ip.dstIP), dstPort)
	go b.connectTCP(flow)
	return flow
}

func (b *bridge) connectTCP(flow *tcpFlow) {
	conn, err := net.DialTimeout("tcp4", fmt.Sprintf("%s:%d", ipText(flow.remoteIP), flow.remotePort), 10*time.Second)
	if err != nil {
		if b.getTCP(flow.key) == flow {
			b.sendTCP(flow, 0x14, nil)
			b.closeTCP(flow.key, false)
		}
		return
	}
	if b.getTCP(flow.key) != flow {
		_ = conn.Close()
		return
	}
	flow.mu.Lock()
	flow.conn = conn
	flow.connected = true
	pending := flow.pending
	flow.pending = nil
	flow.timer = time.NewTicker(10 * time.Second)
	timer := flow.timer
	flow.mu.Unlock()
	for _, p := range pending {
		_, _ = conn.Write(p)
	}
	go b.readTCP(flow, conn)
	go func() {
		for range timer.C {
			flow.mu.Lock()
			lastActive := flow.lastActive
			flow.mu.Unlock()
			if time.Since(lastActive) > tcpIdle {
				b.closeTCP(flow.key, true)
				return
			}
		}
	}()
}

func (b *bridge) readTCP(flow *tcpFlow, conn net.Conn) {
	buf := make([]byte, 4096)
	for {
		n, err := conn.Read(buf)
		if n > 0 {
			for off := 0; off < n; off += tcpMSS {
				end := min(n, off+tcpMSS)
				part := buf[off:end]
				b.sendTCP(flow, 0x18, part)
				flow.mu.Lock()
				flow.serverNext += uint32(len(part))
				flow.mu.Unlock()
			}
		}
		if err != nil {
			b.sendTCP(flow, 0x11, nil)
			flow.mu.Lock()
			flow.serverNext++
			flow.mu.Unlock()
			b.closeTCP(flow.key, false)
			return
		}
	}
}

func (b *bridge) sendTCP(flow *tcpFlow, flags byte, data []byte) {
	flow.mu.Lock()
	remoteIP := clone(flow.remoteIP)
	guestIP := clone(flow.guestIP)
	guestMAC := clone(flow.guestMAC)
	remotePort := flow.remotePort
	guestPort := flow.guestPort
	serverNext := flow.serverNext
	clientNext := flow.clientNext
	flow.mu.Unlock()
	tcp := tcpPacket(remoteIP, guestIP, remotePort, guestPort, serverNext, clientNext, flags, data)
	b.sendEthernet(ethernet(guestMAC, gatewayMAC, ethIPv4, tcp))
}

func (b *bridge) closeTCP(key string, destroy bool) {
	b.flowsMu.Lock()
	flow := b.tcp[key]
	if flow != nil {
		delete(b.tcp, key)
	}
	b.flowsMu.Unlock()
	if flow == nil {
		return
	}
	flow.mu.Lock()
	timer := flow.timer
	conn := flow.conn
	flow.timer = nil
	flow.conn = nil
	flow.connected = false
	flow.mu.Unlock()
	if timer != nil {
		timer.Stop()
	}
	if destroy && conn != nil {
		_ = conn.Close()
	}
}

func parseIPv4(frame []byte) *ipv4Packet {
	if len(frame) < 34 || frame[14]>>4 != 4 {
		return nil
	}
	ihl := int(frame[14]&0x0f) * 4
	total := int(be16(frame[16:18]))
	if ihl < 20 || total < ihl || len(frame) < 14+total {
		return nil
	}
	return &ipv4Packet{
		srcMAC:  clone(frame[6:12]),
		dstMAC:  clone(frame[0:6]),
		srcIP:   clone(frame[26:30]),
		dstIP:   clone(frame[30:34]),
		proto:   frame[23],
		payload: clone(frame[14+ihl : 14+total]),
	}
}

func (b *bridge) sendIP(dstMAC, srcIP, dstIP []byte, proto byte, payload []byte) {
	b.sendEthernet(ethernet(dstMAC, gatewayMAC, ethIPv4, b.ipv4(srcIP, dstIP, proto, payload)))
}

func (b *bridge) ipv4(srcIP, dstIP []byte, proto byte, payload []byte) []byte {
	packet := make([]byte, 20+len(payload))
	packet[0], packet[8], packet[9] = 0x45, 64, proto
	put16(packet[2:4], uint16(len(packet)))
	put16(packet[4:6], b.ipID)
	b.ipID++
	copy(packet[12:16], srcIP)
	copy(packet[16:20], dstIP)
	copy(packet[20:], payload)
	put16(packet[10:12], checksum(packet[:20]))
	return packet
}

func udpPacket(srcIP, dstIP []byte, srcPort, dstPort uint16, body []byte) []byte {
	udp := make([]byte, 8+len(body))
	put16(udp[0:2], srcPort)
	put16(udp[2:4], dstPort)
	put16(udp[4:6], uint16(len(udp)))
	copy(udp[8:], body)
	put16(udp[6:8], transportChecksum(17, srcIP, dstIP, udp))
	packet := make([]byte, 20+len(udp))
	packet[0], packet[8], packet[9] = 0x45, 64, 17
	put16(packet[2:4], uint16(len(packet)))
	copy(packet[12:16], srcIP)
	copy(packet[16:20], dstIP)
	copy(packet[20:], udp)
	put16(packet[10:12], checksum(packet[:20]))
	return packet
}

func tcpPacket(srcIP, dstIP []byte, srcPort, dstPort uint16, seq, ack uint32, flags byte, body []byte) []byte {
	tcp := make([]byte, 20+len(body))
	put16(tcp[0:2], srcPort)
	put16(tcp[2:4], dstPort)
	put32(tcp[4:8], seq)
	put32(tcp[8:12], ack)
	tcp[12], tcp[13] = 5<<4, flags
	put16(tcp[14:16], 8192)
	copy(tcp[20:], body)
	put16(tcp[16:18], transportChecksum(6, srcIP, dstIP, tcp))
	packet := make([]byte, 20+len(tcp))
	packet[0], packet[8], packet[9] = 0x45, 64, 6
	put16(packet[2:4], uint16(len(packet)))
	copy(packet[12:16], srcIP)
	copy(packet[16:20], dstIP)
	copy(packet[20:], tcp)
	put16(packet[10:12], checksum(packet[:20]))
	return packet
}

func ethernet(dstMAC, srcMAC []byte, typ uint16, payload []byte) []byte {
	frame := make([]byte, 14+len(payload))
	copy(frame[0:6], dstMAC)
	copy(frame[6:12], srcMAC)
	put16(frame[12:14], typ)
	copy(frame[14:], payload)
	return frame
}

func dnsResponse(query []byte, answers []net.IP, rcode byte) []byte {
	end := dnsQuestionEnd(query)
	if end == 0 {
		return nil
	}
	resp := make([]byte, end+len(answers)*16)
	copy(resp, query[:end])
	resp[2] = 0x80 | (query[2] & 0x79)
	resp[3] = 0x80 | (rcode & 0x0f)
	put16(resp[6:8], uint16(len(answers)))
	put16(resp[8:10], 0)
	put16(resp[10:12], 0)
	off := end
	for _, ip := range answers {
		resp[off], resp[off+1] = 0xc0, 0x0c
		put16(resp[off+2:off+4], 1)
		put16(resp[off+4:off+6], 1)
		put32(resp[off+6:off+10], 60)
		put16(resp[off+10:off+12], 4)
		copy(resp[off+12:off+16], ip.To4())
		off += 16
	}
	return resp
}

type dnsQ struct {
	name   string
	qtype  uint16
	qclass uint16
}

func dnsQuestion(query []byte) dnsQ {
	var labels []string
	off := 12
	for off < len(query) {
		l := int(query[off])
		off++
		if l == 0 {
			break
		}
		if l&0xc0 != 0 || off+l > len(query) {
			return dnsQ{}
		}
		labels = append(labels, string(query[off:off+l]))
		off += l
	}
	if off+4 > len(query) || len(labels) == 0 {
		return dnsQ{}
	}
	return dnsQ{name: strings.Join(labels, "."), qtype: be16(query[off : off+2]), qclass: be16(query[off+2 : off+4])}
}

func dnsQuestionEnd(query []byte) int {
	off := 12
	for off < len(query) {
		l := int(query[off])
		off++
		if l == 0 {
			break
		}
		if l&0xc0 != 0 || off+l > len(query) {
			return 0
		}
		off += l
	}
	if off+4 > len(query) {
		return 0
	}
	return off + 4
}

func dhcpType(payload []byte) byte {
	for off := 240; off < len(payload); {
		code := payload[off]
		off++
		if code == 0 {
			continue
		}
		if code == 255 || off >= len(payload) {
			break
		}
		l := int(payload[off])
		off++
		if off+l > len(payload) {
			break
		}
		if code == 53 && l > 0 {
			return payload[off]
		}
		off += l
	}
	return 0
}

func dhcpOpt(buf []byte, off int, code byte, value []byte) int {
	buf[off] = code
	buf[off+1] = byte(len(value))
	copy(buf[off+2:], value)
	return off + 2 + len(value)
}

func transportChecksum(proto byte, srcIP, dstIP, segment []byte) uint16 {
	pseudo := make([]byte, 12+len(segment)+len(segment)%2)
	copy(pseudo[0:4], srcIP)
	copy(pseudo[4:8], dstIP)
	pseudo[9] = proto
	put16(pseudo[10:12], uint16(len(segment)))
	copy(pseudo[12:], segment)
	return checksum(pseudo)
}

func checksum(buf []byte) uint16 {
	var sum uint32
	for i := 0; i+1 < len(buf); i += 2 {
		sum += uint32(be16(buf[i : i+2]))
	}
	if len(buf)%2 != 0 {
		sum += uint32(buf[len(buf)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = (sum & 0xffff) + (sum >> 16)
	}
	return ^uint16(sum)
}

func ip4(s string) []byte {
	ip := net.ParseIP(s).To4()
	if ip == nil {
		panic(s)
	}
	return []byte(ip)
}

func ipText(ip []byte) string {
	return net.IP(ip).String()
}

func macText(mac []byte) string {
	parts := make([]string, len(mac))
	for i, b := range mac {
		parts[i] = fmt.Sprintf("%02x", b)
	}
	return strings.Join(parts, ":")
}

func be16(b []byte) uint16 {
	return binary.BigEndian.Uint16(b)
}

func be32(b []byte) uint32 {
	return binary.BigEndian.Uint32(b)
}

func put16(b []byte, v uint16) {
	binary.BigEndian.PutUint16(b, v)
}

func put32(b []byte, v uint32) {
	binary.BigEndian.PutUint32(b, v)
}

func eq(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func clone(b []byte) []byte {
	out := make([]byte, len(b))
	copy(out, b)
	return out
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
