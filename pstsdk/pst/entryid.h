#ifndef PSTSDK_PST_ENTRYID_H
#define PSTSDK_PST_ENTRYID_H

#include "pstsdk/util/primitives.h"
#include "pstsdk/util/util.h"
#include <cstdint>
#include <vector>

namespace pstsdk {

enum class distribution_list_entry_id_type {
	one_off = 0,
	contact = 3,
	personal_distribution_list = 4,
	mail_user_gal = 5,
	distribution_list_gal = 6
};

#pragma pack(push, 1)
struct entry_id_header {
	const uint32_t flags;
	const guid provider_uid;
};

struct entry_id_bytebag : entry_id_header {
	byte data[];
};

struct distribution_list_wrapped_entry_id : entry_id_header {
	uint8_t type;
	byte data[];

	inline distribution_list_entry_id_type get_type() {
		return static_cast<distribution_list_entry_id_type>(type & 0x0F);
	}
};

/**
 * @brief One-off recipient EntryID
 * [MS-OXCDATA] 2.2.5.1
 */
struct recipient_oneoff_entry_id : entry_id_header {
	const uint16_t version;
	const uint8_t pad;
	const uint16_t meta;
	const byte data[];

	inline bool is_mime() const {
		return (meta & 0x0100);
	}

	inline bool is_unicode() const {
		return (meta & 0x0080);
	}

	inline std::vector<std::string> read_strings() const {
		std::vector<std::string> strings;
		const char *begin = reinterpret_cast<const char *>(&data[0]);

		while (strings.size() < 3) {
			size_t length = 0;
			if (is_unicode()) {
				for (int i = 0; i < 256; i += 2) {
					const uint16_t *wc = reinterpret_cast<const uint16_t *>(begin + i);
					if (*wc == 0) {
						length = i + 2;
						break;
					}
				}

			} else {
				length = strnlen(begin, 256) + 1;
			}

			if (length == 0)
				break;

			std::vector<pstsdk::byte> bytes(begin, begin + length);
			strings.emplace_back(bytes_to_string(bytes));
			begin = begin + length;
		}

		return strings;
	}
};
#pragma pack(pop)

} // namespace pstsdk

#endif