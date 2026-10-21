package main

import (
	"bufio"
	"bytes"
	"io"
	"net"
	"testing"
	"time"
)

type tcpFixture struct {
	b       *bridge
	flow    *tcpFlow
	host    net.Conn
	packets chan []byte
}

func newTCPFixture(t *testing.T, window uint32) *tcpFixture {
	t.Helper()
	ws, peer := net.Pipe()
	remote, host := net.Pipe()
	f := &tcpFlow{key: "10.0.0.1:1234>1.2.3.4:443", conn: remote, connected: true,
		guestMAC: []byte{16, 32, 48, 0, 0, 16}, guestIP: clone(guestIP), guestPort: 1234,
		remoteIP: []byte{1, 2, 3, 4}, remotePort: 443, clientNext: 500, serverNext: 1001,
		serverAck: 1001, serverISN: 1000, peerWindow: window, ackWake: make(chan struct{}, 1), done: make(chan struct{}), lastActive: time.Now()}
	b := &bridge{client: ws, tcp: map[string]*tcpFlow{f.key: f}}
	fx := &tcpFixture{b: b, flow: f, host: host, packets: make(chan []byte, 100)}
	t.Cleanup(func() { b.closeTCP(f.key, true); ws.Close(); peer.Close(); host.Close(); remote.Close() })
	go func() {
		r := bufio.NewReader(peer)
		for {
			_, frame, err := wsReadFrame(r)
			if err != nil {
				return
			}
			fx.packets <- parseIPv4(frame).payload
		}
	}()
	// Establish the advertised window through the real packet handler.
	fx.ack(1001, uint16(window))
	go b.readTCP(f, remote)
	return fx
}
func (f *tcpFixture) ack(seq uint32, window uint16) {
	p := tcpPacket(f.flow.guestIP, f.flow.remoteIP, 1234, 443, 500, seq, 0x10, nil)
	put16(p[20+14:20+16], window)
	f.b.handleTCP(parseIPv4(ethernet(gatewayMAC, f.flow.guestMAC, ethIPv4, p)))
}
func (f *tcpFixture) packet(t *testing.T) []byte {
	t.Helper()
	select {
	case p := <-f.packets:
		return p
	case <-time.After(2 * time.Second):
		t.Fatal("no TCP packet")
		return nil
	}
}
func TestTCPRespectsWindowAndRetransmits(t *testing.T) {
	f := newTCPFixture(t, 1000)
	data := bytes.Repeat([]byte("a"), 4096)
	go f.host.Write(data)
	p := f.packet(t)
	if len(p)-20 != 1000 {
		t.Fatalf("first payload %d, window 1000", len(p)-20)
	}
	// Drop the packet: the same bytes must be retransmitted, not new data.
	retry := f.packet(t)
	if !bytes.Equal(retry, p) {
		t.Fatal("unacknowledged data was not retransmitted")
	}
	f.ack(2001, 0)
	probe := f.packet(t)
	if len(probe) != 21 || be32(probe[4:8]) != 2000 {
		t.Fatalf("invalid zero-window probe: seq %d len %d", be32(probe[4:8]), len(probe))
	}
	f.ack(2001, 2000)
	next := f.packet(t)
	if be32(next[4:8]) != 2001 {
		t.Fatal("did not resume after window reopened")
	}
	// Drain and ACK the whole transfer, including a final partial segment.
	received := 1000
	for received < len(data) {
		if be32(next[4:8]) != 1001+uint32(received) {
			t.Fatalf("unexpected sequence %d", be32(next[4:8]))
		}
		received += len(next) - 20
		f.ack(1001+uint32(received), 2000)
		if received < len(data) {
			next = f.packet(t)
		}
	}
}
func TestTCPDoesNotForwardDuplicateOrOutOfOrderBytes(t *testing.T) {
	f := newTCPFixture(t, 4096)
	var got bytes.Buffer
	done := make(chan struct{})
	go func() { io.Copy(&got, f.host); close(done) }()
	send := func(seq uint32, data string) {
		p := tcpPacket(f.flow.guestIP, f.flow.remoteIP, 1234, 443, seq, 1001, 0x18, []byte(data))
		f.b.handleTCP(parseIPv4(ethernet(gatewayMAC, f.flow.guestMAC, ethIPv4, p)))
	}
	send(500, "hello")
	send(500, "hello")
	send(510, "wrong")
	send(503, "lo world")
	f.host.Close()
	<-done
	if got.String() != "hello world" {
		t.Fatalf("host received %q", got.String())
	}
}

func TestTCPRetransmitsFINUntilAcknowledged(t *testing.T) {
	f := newTCPFixture(t, 4096)
	f.host.Close()
	first := f.packet(t)
	if first[13]&1 == 0 || be32(first[4:8]) != 1001 {
		t.Fatal("missing FIN")
	}
	retry := f.packet(t)
	if !bytes.Equal(first, retry) {
		t.Fatal("FIN was not retransmitted")
	}
	f.ack(1002, 4096)
	select {
	case <-f.flow.done:
	case <-time.After(time.Second):
		t.Fatal("flow not closed after FIN acknowledgement")
	}
}
func TestTCPRepeatsSYNACKWithoutAdvancingSequence(t *testing.T) {
	f := newTCPFixture(t, 4096)
	p := tcpPacket(f.flow.guestIP, f.flow.remoteIP, 1234, 443, 499, 0, 0x02, nil)
	ip := parseIPv4(ethernet(gatewayMAC, f.flow.guestMAC, ethIPv4, p))
	f.b.handleTCP(ip)
	first := f.packet(t)
	f.b.handleTCP(ip)
	retry := f.packet(t)
	if be32(first[4:8]) != 1000 || !bytes.Equal(first, retry) {
		t.Fatal("retransmitted SYN changed SYN-ACK sequence")
	}
}
func TestWebSocketFramesStayIntactAcrossConcurrentFlows(t *testing.T) {
	server, peer := net.Pipe()
	defer server.Close()
	defer peer.Close()
	b := &bridge{client: server}
	for i := 0; i < 8; i++ {
		go func(i int) {
			for j := 0; j < 10; j++ {
				b.sendEthernet(bytes.Repeat([]byte{byte(i)}, 1400))
			}
		}(i)
	}
	peer.SetReadDeadline(time.Now().Add(2 * time.Second))
	r := bufio.NewReader(peer)
	counts := [8]int{}
	for i := 0; i < 80; i++ {
		op, p, err := wsReadFrame(r)
		if err != nil {
			t.Fatal(err)
		}
		if op != 2 || len(p) != 1400 || p[0] >= 8 || !bytes.Equal(p, bytes.Repeat(p[:1], 1400)) {
			t.Fatal("interleaved WebSocket frames")
		}
		counts[p[0]]++
	}
	for _, n := range counts {
		if n != 10 {
			t.Fatal("lost WebSocket frame")
		}
	}
}
