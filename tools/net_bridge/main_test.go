package main

import (
	"net"
	"testing"
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
	copy(req[236:240], []byte{99, 130, 83, 99})
	got := dhcpReplyMAC(ip, req)
	if !eq(got, bcastMAC) {
		t.Fatalf("dhcpReplyMAC = %s, want broadcast", macText(got))
	}
}
