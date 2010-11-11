/*
 * CTR with CBC-MAC Protocol (CCMP)
 * Copyright (c) 2010, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "common/ieee802_11_defs.h"
#include "crypto/aes.h"
#include "wlantest.h"


static void ccmp_aad_nonce(const struct ieee80211_hdr *hdr, const u8 *data,
			   u8 *aad, size_t *aad_len, u8 *nonce)
{
	u16 fc, stype, seq;
	int qos = 0, addr4 = 0;
	u8 *pos;

	nonce[0] = 0;

	fc = le_to_host16(hdr->frame_control);
	stype = WLAN_FC_GET_STYPE(fc);
	if ((fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) ==
	    (WLAN_FC_TODS | WLAN_FC_FROMDS))
		addr4 = 1;

	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA) {
		fc &= ~0x0070; /* Mask subtype bits */
		if (stype & 0x08) {
			const u8 *qc;
			qos = 1;
			fc &= ~WLAN_FC_ORDER;
			qc = (const u8 *) (hdr + 1);
			if (addr4)
				qc += ETH_ALEN;
			nonce[0] = qc[0] & 0x0f;
		}
	} else if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_MGMT)
		nonce[0] |= 0x10; /* Management */

	fc &= ~(WLAN_FC_RETRY | WLAN_FC_PWRMGT | WLAN_FC_MOREDATA);
	fc |= WLAN_FC_ISWEP;
	WPA_PUT_LE16(aad, fc);
	pos = aad + 2;
	os_memcpy(pos, hdr->addr1, 3 * ETH_ALEN);
	pos += 3 * ETH_ALEN;
	seq = le_to_host16(hdr->seq_ctrl);
	seq &= ~0xfff0; /* Mask Seq#; do not modify Frag# */
	WPA_PUT_LE16(pos, seq);
	pos += 2;

	os_memcpy(pos, hdr + 1, addr4 * ETH_ALEN + qos * 2);
	pos += addr4 * ETH_ALEN;
	if (qos) {
		pos[0] &= 0x70;
		if (1 /* FIX: either device has SPP A-MSDU Capab = 0 */)
			pos[0] &= 0x80;
		pos++;
		*pos++ = 0x00;
	}

	*aad_len = pos - aad;

	os_memcpy(nonce + 1, hdr->addr2, ETH_ALEN);
	nonce[7] = data[7]; /* PN5 */
	nonce[8] = data[6]; /* PN4 */
	nonce[9] = data[5]; /* PN3 */
	nonce[10] = data[4]; /* PN2 */
	nonce[11] = data[1]; /* PN1 */
	nonce[12] = data[0]; /* PN0 */
}


static void xor_aes_block(u8 *dst, const u8 *src)
{
	u32 *d = (u32 *) dst;
	u32 *s = (u32 *) src;
	*d++ ^= *s++;
	*d++ ^= *s++;
	*d++ ^= *s++;
	*d++ ^= *s++;
}


u8 * ccmp_decrypt(const u8 *tk, const struct ieee80211_hdr *hdr,
		  const u8 *data, size_t data_len, size_t *decrypted_len)
{
	u8 aad[2 + 30], nonce[13];
	size_t aad_len;
	u8 b[AES_BLOCK_SIZE], x[AES_BLOCK_SIZE], a[AES_BLOCK_SIZE];
	void *aes;
	const u8 *m, *mpos, *mic;
	size_t mlen, last;
	int i;
	u8 *plain, *ppos;
	u8 t[8];

	if (data_len < 8 + 8)
		return NULL;

	plain = os_malloc(data_len);
	if (plain == NULL)
		return NULL;

	aes = aes_encrypt_init(tk, 16);
	if (aes == NULL) {
		os_free(plain);
		return NULL;
	}

	m = data + 8;
	mlen = data_len - 8 - 8;
	last = mlen % AES_BLOCK_SIZE;

	os_memset(aad, 0, sizeof(aad));
	ccmp_aad_nonce(hdr, data, &aad[2], &aad_len, nonce);
	WPA_PUT_BE16(aad, aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP AAD", &aad[2], aad_len);
	wpa_hexdump(MSG_EXCESSIVE, "CCMP nonce", nonce, 13);

	/* CCM: M=8 L=2, Adata=1, M' = (M-2)/2 = 3, L' = L-1 = 1 */

	/* A_i = Flags | Nonce N | Counter i */
	a[0] = 0x01; /* Flags = L' */
	os_memcpy(&a[1], nonce, 13);

	/* Decryption */

	mic = data + data_len - 8;
	wpa_hexdump(MSG_EXCESSIVE, "CCMP U", mic, 8);
	/* U = T XOR S_0; S_0 = E(K, A_0) */
	WPA_PUT_BE16(&a[14], 0);
	aes_encrypt(aes, a, x);
	for (i = 0; i < 8; i++)
		t[i] = mic[i] ^ x[i];
	wpa_hexdump(MSG_EXCESSIVE, "CCMP T", t, 8);

	/* plaintext = msg XOR (S_1 | S_2 | ... | S_n) */
	ppos = plain;
	mpos = m;
	for (i = 1; i <= mlen / AES_BLOCK_SIZE; i++) {
		WPA_PUT_BE16(&a[14], i);
		/* S_i = E(K, A_i) */
		aes_encrypt(aes, a, ppos);
		xor_aes_block(ppos, mpos);
		ppos += AES_BLOCK_SIZE;
		mpos += AES_BLOCK_SIZE;
	}
	if (last) {
		WPA_PUT_BE16(&a[14], i);
		aes_encrypt(aes, a, ppos);
		/* XOR zero-padded last block */
		for (i = 0; i < last; i++)
			*ppos++ ^= *mpos++;
	}
	wpa_hexdump(MSG_EXCESSIVE, "CCMP decrypted", plain, mlen);

	/* Authentication */
	/* B_0: Flags | Nonce N | l(m) */
	b[0] = 0x40 /* Adata */ | (3 /* M' */ << 3) | 1 /* L' */;
	os_memcpy(&b[1], nonce, 13);
	WPA_PUT_BE16(&b[14], mlen);

	wpa_hexdump(MSG_EXCESSIVE, "CCMP B_0", b, AES_BLOCK_SIZE);
	aes_encrypt(aes, b, x); /* X_1 = E(K, B_0) */

	wpa_hexdump(MSG_EXCESSIVE, "CCMP B_1", aad, AES_BLOCK_SIZE);
	xor_aes_block(aad, x);
	aes_encrypt(aes, aad, x); /* X_2 = E(K, X_1 XOR B_1) */

	wpa_hexdump(MSG_EXCESSIVE, "CCMP B_2", &aad[AES_BLOCK_SIZE],
		    AES_BLOCK_SIZE);
	xor_aes_block(&aad[AES_BLOCK_SIZE], x);
	aes_encrypt(aes, &aad[AES_BLOCK_SIZE], x); /* X_3 = E(K, X_2 XOR B_2)
						    */

	ppos = plain;
	for (i = 0; i < mlen / AES_BLOCK_SIZE; i++) {
		/* X_i+1 = E(K, X_i XOR B_i) */
		xor_aes_block(x, ppos);
		ppos += AES_BLOCK_SIZE;
		aes_encrypt(aes, x, x);
	}
	if (last) {
		/* XOR zero-padded last block */
		for (i = 0; i < last; i++)
			x[i] ^= *ppos++;
		aes_encrypt(aes, x, x);
	}

	aes_encrypt_deinit(aes);

	if (os_memcmp(x, t, 8) != 0) {
		wpa_printf(MSG_INFO, "Invalid CCMP MIC in frame from " MACSTR,
			   MAC2STR(hdr->addr2));
		os_free(plain);
		return NULL;
	}

	*decrypted_len = mlen;
	return plain;
}