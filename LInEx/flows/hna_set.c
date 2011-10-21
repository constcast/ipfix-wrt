#include "hna_set.h"

struct hna_set *find_or_create_hna_set(node_set_hash *node_set,
									   struct ip_addr_t *addr) {
	struct node_entry *node_entry = find_or_create_node_entry(node_set,
															  addr);

	if (!node_entry->hna_set) {
		// Create new entry
		struct hna_set *hs = (struct hna_set *) malloc(sizeof(struct hna_set));
		hs->first = hs->last = NULL;
		hs->protocol = addr->protocol;

		node_entry->hna_set = hs;
	}

	return node_entry->hna_set;
}

struct hna_set_entry *find_or_create_hna_set_entry(struct hna_set *hs,
												   union olsr_ip_addr *addr, uint8_t netmask) {
	struct hna_set_entry *hs_entry = hs->first;

	while (hs_entry != NULL) {
		if (hs->protocol == IPv4) {
			if (hs_entry->network.v4.s_addr == addr->v4.s_addr
					&& hs_entry->netmask == netmask)
				break;
		} else {
#ifdef SUPPORT_IPV6
			if (memcmp(&hs_entry->network.v6, &addr->v6, sizeof(addr->v6))
					&& hs_entry->netmask == netmask)
				break;
#endif
		}

		hs_entry = hs_entry->next;
	}

	if (hs_entry == NULL) {
		hs_entry = (struct hna_set_entry *) malloc (sizeof(struct hna_set_entry));
		hs_entry->next = NULL;

		if (hs_entry == NULL)
			return NULL;

		hs_entry->network = *addr;
		hs_entry->netmask = netmask;

		if (hs->last != NULL)
			hs->last->next = hs_entry;
		if (hs->first == NULL)
			hs->first = hs_entry;

		hs->last = hs_entry;
	}

	return hs_entry;
}

struct hna_set_entry *hna_set_remove_entry(struct hna_set *set,
										   struct hna_set_entry *entry,
										   struct hna_set_entry *previous_entry) {
	struct hna_set_entry *next = entry->next;

	if (entry == set->first)
		set->first = entry->next;

	if (entry == set->last)
		set->last = previous_entry;

	if (previous_entry)
		previous_entry->next = entry->next;

	free(entry);

	return next;
}

void expire_hna_set_entries(struct hna_set *set, time_t now) {
	struct hna_set_entry *entry = set->first;
	struct hna_set_entry *previous_entry = NULL;

	while (entry != NULL) {
		if (entry->vtime < now) {
			entry = hna_set_remove_entry(set, entry, previous_entry);
		} else {
			previous_entry = entry;
			entry = entry->next;
		}
	}
}