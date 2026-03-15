/*
 * WireGuard - Public API header
 * Copyright (C) 2015-2024 Jason A. Donenfeld <Jason@zx2c4.com>
 * Copyright (C) 2026 Port to kernel 3.10.108
 */

#ifndef _WG_NET_WIREGUARD_H
#define _WG_NET_WIREGUARD_H

#include <linux/types.h>
#include <uapi/linux/wireguard.h>

/* WireGuard message types */
enum message_type {
	MESSAGE_HANDSHAKE_INITIATION = 1,
	MESSAGE_HANDSHAKE_RESPONSE = 2,
	MESSAGE_HANDSHAKE_COOKIE = 3,
	MESSAGE_DATA = 4
};

/* Message structures */
struct message_handshake_initiation {
	__le32 message_type;
	__le32 sender_index;
	u8 ephemeral[32];
	u8 static_pub[32];
	u8 timestamp[12];
	u8 mac1[16];
	u8 mac2[16];
};

struct message_handshake_response {
	__le32 message_type;
	__le32 sender_index;
	__le32 receiver_index;
	u8 ephemeral[32];
	u8 empty[32];
	u8 mac1[16];
};

struct message_handshake_cookie {
	__le32 message_type;
	__le32 receiver_index;
	u8 nonce[24];
	u8 cookie[16];
	u8 mac1[16];
};

struct message_data {
	__le32 message_type;
	__le32 receiver_index;
	__le64 counter;
	u8 data[];
};

#endif /* _WG_NET_WIREGUARD_H */
