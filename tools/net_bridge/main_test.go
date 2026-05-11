package main

import (
	"bufio"
	"net"
	"testing"
	"time"
)

func dnsQueryForTest(name string, qtype uint16, additional bool) []byte {
	query := []byte{
		0x12, 0x34,
		0x01, 0x00,
		0x00, 0x01,
		0x00, 0x00,
		0x00, 0x00,
		0x00, 0x00,
	}
	for _, label := range []string{"frogfind", "com"} {
		query = append(query, byte(len(label)))
		query = append(query, []byte(label)...)
	}
	query = append(query, 0, byte(qtype>>8), byte(qtype), 0, 1)
	if additional {
		query[11] = 1
		query = append(query,
			0x00,
			0x00, 0x29,
			0x04, 0xd0,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00)
	}
	return query
}

func TestDNSResponseNoDataIsNotNXDOMAIN(t *testing.T) {
	resp := dnsResponse(dnsQueryForTest("frogfind.com", 28, true), nil, 0)
	if len(resp) == 0 {
		t.Fatal("empty DNS response")
	}
	if got := resp[3] & 0x0f; got != 0 {
		t.Fatalf("rcode = %d, want 0", got)
	}
	if got := be16(resp[6:8]); got != 0 {
		t.Fatalf("ancount = %d, want 0", got)
	}
	if got := be16(resp[10:12]); got != 0 {
		t.Fatalf("arcount = %d, want 0", got)
	}
	if got, want := len(resp), dnsQuestionEnd(resp); got != want {
		t.Fatalf("response length = %d, want question-only length %d", got, want)
	}
}

func TestDNSResponseARecordClearsAdditionalCount(t *testing.T) {
	resp := dnsResponse(
		dnsQueryForTest("frogfind.com", 1, true),
		[]net.IP{net.IPv4(1, 2, 3, 4)},
		0,
	)
	if got := resp[3] & 0x0f; got != 0 {
		t.Fatalf("rcode = %d, want 0", got)
	}
	if got := be16(resp[6:8]); got != 1 {
		t.Fatalf("ancount = %d, want 1", got)
	}
	if got := be16(resp[10:12]); got != 0 {
		t.Fatalf("arcount = %d, want 0", got)
	}
}

func TestDHCPReplyMACUsesBroadcastWhenClientBroadcasts(t *testing.T) {
	ip := &ipv4Packet{
		srcMAC: []byte{0x10, 0x20, 0x30, 0x00, 0x00, 0x10},
		dstMAC: clone(bcastMAC),
		srcIP:  clone(zeroIP),
	}
	req := make([]byte, 240)
	req[10], req[11] = 0x80, 0x00
	copy(req[236:240], []byte{99, 130, 83, 99})
	got := dhcpReplyMAC(ip, req)
	if !eq(got, bcastMAC) {
		t.Fatalf("dhcpReplyMAC = %s, want broadcast", macText(got))
	}
}

func TestDHCPRenewalReplyUsesClientIP(t *testing.T) {
	req := make([]byte, 240)
	copy(req[12:16], guestIP)
	copy(req[236:240], []byte{99, 130, 83, 99})
	got := dhcpReplyIP(req)
	if !eq(got, guestIP) {
		t.Fatalf("dhcpReplyIP = %s, want %s", ipText(got), ipText(guestIP))
	}
}

func TestDHCPInitRebootReplyUsesBroadcastIPWhenCIAddrIsZero(t *testing.T) {
	req := make([]byte, 248)
	copy(req[236:240], []byte{99, 130, 83, 99})
	copy(req[240:], []byte{50, 4, 10, 0, 0, 1, 255, 0})
	got := dhcpReplyIP(req)
	if !eq(got, bcastIP) {
		t.Fatalf("dhcpReplyIP = %s, want %s", ipText(got), ipText(bcastIP))
	}
}

func TestDHCPInitRebootACKFrameUsesClientMACAndBroadcastIP(t *testing.T) {
	clientMAC := []byte{0x10, 0x20, 0x30, 0x00, 0x00, 0x10}
	ip := &ipv4Packet{
		srcMAC: clone(clientMAC),
		dstMAC: clone(bcastMAC),
		srcIP:  clone(zeroIP),
		dstIP:  clone(bcastIP),
	}
	req := make([]byte, 253)
	copy(req[28:34], clientMAC)
	copy(req[236:240], []byte{99, 130, 83, 99})
	copy(req[240:], []byte{
		53, 1, 3,
		50, 4, 10, 0, 0, 1,
		55, 3, 1, 3, 6,
		255,
	})

	server, peer := net.Pipe()
	defer server.Close()
	defer peer.Close()
	b := &bridge{client: server}
	done := make(chan struct{})
	go func() {
		b.handleDHCP(ip, req)
		close(done)
	}()

	if err := peer.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	opcode, frame, err := wsReadFrame(bufio.NewReader(peer))
	if err != nil {
		t.Fatal(err)
	}
	<-done
	if opcode != 0x2 {
		t.Fatalf("opcode = 0x%x, want binary", opcode)
	}
	if len(frame) < 14+20+8+241 {
		t.Fatalf("short DHCP frame: %d bytes", len(frame))
	}
	if !eq(frame[0:6], clientMAC) {
		t.Fatalf("eth dst = %s, want %s", macText(frame[0:6]), macText(clientMAC))
	}
	if !eq(frame[30:34], bcastIP) {
		t.Fatalf("ip dst = %s, want %s", ipText(frame[30:34]), ipText(bcastIP))
	}
	if got := be16(frame[36:38]); got != 68 {
		t.Fatalf("udp dst = %d, want 68", got)
	}
	dhcp := frame[14+20+8:]
	if got := dhcpType(dhcp); got != 5 {
		t.Fatalf("dhcp type = %d, want ACK", got)
	}
	if got := string(dhcpOption(dhcp, 15)); got != "local" {
		t.Fatalf("domain option = %q, want local", got)
	}
	if got := dhcpOption(dhcp, 44); !eq(got, gatewayIP) {
		t.Fatalf("netbios name server = %s, want %s", ipText(got), ipText(gatewayIP))
	}
	if got := dhcpOption(dhcp, 46); len(got) != 1 || got[0] != 8 {
		t.Fatalf("netbios node type = %v, want [8]", got)
	}
}

func TestEthernetPadsShortFrames(t *testing.T) {
	frame := ethernet(
		[]byte{1, 2, 3, 4, 5, 6},
		[]byte{6, 5, 4, 3, 2, 1},
		ethARP,
		make([]byte, 28),
	)
	if got := len(frame); got != minEthernetFrame {
		t.Fatalf("ethernet frame length = %d, want %d", got, minEthernetFrame)
	}
	if got := be16(frame[12:14]); got != ethARP {
		t.Fatalf("ethernet type = 0x%x, want ARP", got)
	}
}

func TestDNSResponseCanUseArbitraryServerIP(t *testing.T) {
	serverIP := []byte{8, 8, 8, 8}
	guestIP := []byte{10, 0, 0, 1}
	dns := dnsResponse(
		dnsQueryForTest("frogfind.com", 1, false),
		[]net.IP{net.IPv4(1, 2, 3, 4)},
		0,
	)
	udp := udpPacket(serverIP, guestIP, 53, 12345, dns)
	if !eq(udp[12:16], serverIP) {
		t.Fatalf("DNS reply source IP = %s, want %s", ipText(udp[12:16]), ipText(serverIP))
	}
}
