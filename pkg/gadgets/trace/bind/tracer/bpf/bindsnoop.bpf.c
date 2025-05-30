/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */
/* Copyright (c) 2021 Hengqi Chen */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#include "bindsnoop.h"
#include <gadget/mntns.h>
#include <gadget/mntns_filter.h>

#define MAX_ENTRIES 10240
#define MAX_PORTS 1024

const volatile pid_t target_pid = 0;
const volatile bool ignore_errors = true;
const volatile bool filter_by_port = false;

// we need this to make sure the compiler doesn't remove our struct
const struct bind_event *unusedbindevent __attribute__((unused));

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, __u32);
	__type(value, struct socket *);
} sockets SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_PORTS);
	__type(key, __u16);
	__type(value, __u16);
} ports SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__uint(key_size, sizeof(__u32));
	__uint(value_size, sizeof(__u32));
} events SEC(".maps");

static int probe_entry(struct pt_regs *ctx, struct socket *socket)
{
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 pid = pid_tgid >> 32;
	__u32 tid = (__u32)pid_tgid;

	if (target_pid && target_pid != pid)
		return 0;

	bpf_map_update_elem(&sockets, &tid, &socket, BPF_ANY);
	return 0;
};

static int probe_exit(struct pt_regs *ctx, short ver)
{
	__u64 pid_tgid = bpf_get_current_pid_tgid();
	__u32 pid = pid_tgid >> 32;
	__u32 tid = (__u32)pid_tgid;
	__u64 uid_gid = bpf_get_current_uid_gid();
	u64 mntns_id;
	struct socket **socketp, *socket;
	struct inet_sock *inet_sock;
	struct sock *sock;
	union bind_options opts;
	struct bind_event event = {};
	__u16 sport = 0, *port;
	int ret;

	socketp = bpf_map_lookup_elem(&sockets, &tid);
	if (!socketp)
		return 0;

	mntns_id = gadget_get_current_mntns_id();

	if (gadget_should_discard_mntns_id(mntns_id))
		goto cleanup;

	ret = PT_REGS_RC(ctx);
	if (ignore_errors && ret != 0)
		goto cleanup;

	socket = *socketp;
	sock = BPF_CORE_READ(socket, sk);
	inet_sock = (struct inet_sock *)sock;

	sport = bpf_ntohs(BPF_CORE_READ(inet_sock, inet_sport));
	port = bpf_map_lookup_elem(&ports, &sport);
	if (filter_by_port && !port)
		goto cleanup;

	opts.fields.freebind = get_inet_sock_freebind(inet_sock);
	opts.fields.transparent = get_inet_sock_transparent(inet_sock);
	opts.fields.bind_address_no_port =
		get_inet_sock_bind_address_no_port(inet_sock);
	opts.fields.reuseaddress =
		BPF_CORE_READ_BITFIELD_PROBED(sock, __sk_common.skc_reuse);
	opts.fields.reuseport =
		BPF_CORE_READ_BITFIELD_PROBED(sock, __sk_common.skc_reuseport);
	event.opts = opts.data;
	event.ts_us = bpf_ktime_get_ns() / 1000;
	event.pid = pid;
	event.port = sport;
	event.bound_dev_if = BPF_CORE_READ(sock, __sk_common.skc_bound_dev_if);
	event.ret = ret;
	event.proto = BPF_CORE_READ_BITFIELD_PROBED(sock, sk_protocol);
	event.mount_ns_id = mntns_id;
	event.timestamp = bpf_ktime_get_boot_ns();
	event.uid = (u32)uid_gid;
	event.gid = (u32)(uid_gid >> 32);
	bpf_get_current_comm(&event.task, sizeof(event.task));
	if (ver == 4) {
		event.ver = ver;
		bpf_probe_read_kernel(&event.addr, sizeof(event.addr),
				      &inet_sock->inet_saddr);
	} else { /* ver == 6 */
		event.ver = ver;
		bpf_probe_read_kernel(
			&event.addr, sizeof(event.addr),
			sock->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
	}
	bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU, &event,
			      sizeof(event));

cleanup:
	bpf_map_delete_elem(&sockets, &tid);
	return 0;
}

SEC("kprobe/inet_bind")
int BPF_KPROBE(ig_bind_ipv4_e, struct socket *socket)
{
	return probe_entry(ctx, socket);
}

SEC("kretprobe/inet_bind")
int BPF_KRETPROBE(ig_bind_ipv4_x)
{
	return probe_exit(ctx, 4);
}

SEC("kprobe/inet6_bind")
int BPF_KPROBE(ig_bind_ipv6_e, struct socket *socket)
{
	return probe_entry(ctx, socket);
}

SEC("kretprobe/inet6_bind")
int BPF_KRETPROBE(ig_bind_ipv6_x)
{
	return probe_exit(ctx, 6);
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
