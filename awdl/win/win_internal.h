/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared between the Windows replacements for OWL's io.c and netutils.c. */
#pragma once
#include <stdint.h>

struct radio_client;

/* The radio connection opened by io_state_init; NULL before and after. */
struct radio_client *winawdl_radio(void);
int winawdl_neighbor_add(int ifindex, const uint8_t address[16], const uint8_t physical[6]);
int winawdl_neighbor_remove(int ifindex, const uint8_t address[16]);

/* Joins the TCP segments of a burst into one packet for the host, waiting
 * `wait_us` after each frame for the next: see tcp_merge.h. 0 turns it off. */
void winawdl_join_segments(int wait_us);
/* How many segments have gone to the host inside another one's packet. */
unsigned long long winawdl_segments_joined(void);
