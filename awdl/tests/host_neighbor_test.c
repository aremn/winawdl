/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Live Windows integration: own temporary Wintun only, no radio transport. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include "host_if.h"

int main(void) {
	const GUID owned = {0x2be941ea,0xa750,0x450a,{0xba,0x41,0xfd,0xd8,0xdf,0x08,0x70,0xb6}};
	NET_LUID existing = {0};
	MIB_IF_ROW2 present = {0};
	if (ConvertInterfaceGuidToLuid(&owned, &existing) == NO_ERROR)
		present.InterfaceLuid = existing;
	if (present.InterfaceLuid.Value && GetIfEntry2(&present) == NO_ERROR) {
		puts("refused: owned adapter already exists");
		return 3;
	}
	const uint8_t self[16] = {0xfe,0x80,0,0,0,0,0,0,0,0xc4,0xc5,0xff,0xfe,0x16,0x17,0x18};
	const uint8_t peer[16] = {0xfe,0x80,0,0,0,0,0,0,0,0xa1,0x12,0xff,0xfe,0x34,0x56,0xab};
	const uint8_t mac[6] = {2,0xa1,0x12,0x34,0x56,0xab};
	char error[200];
	struct host_if *host = host_if_open(self, error, sizeof(error));
	if (!host) {
		printf("host creation failed: %s\n", error);
		return 3;
	}
	if (ConvertInterfaceGuidToLuid(&owned, &existing) != NO_ERROR) {
		host_if_close(host);
		puts("owned adapter identity was not found");
		return 3;
	}
	DWORD code = host_if_neighbor_add(host, peer, mac);
	printf("neighbor creation and permanent-state readback: %lu\n", code);
	if (code == NO_ERROR) {
		code = host_if_neighbor_remove(host, peer);
		printf("neighbor removal: %lu\n", code);
	}
	host_if_close(host);
	BOOL absent = FALSE;
	for (unsigned attempt = 0; attempt < 100; attempt++) {
		present = (MIB_IF_ROW2){0};
		present.InterfaceLuid = existing;
		if (GetIfEntry2(&present) != NO_ERROR) { absent = TRUE; break; }
		Sleep(50);
	}
	printf("owned adapter absent: %s\n", absent ? "true" : "false");
	return code == NO_ERROR && absent ? 0 : 3;
}
