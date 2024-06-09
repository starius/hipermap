#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

extern "C" {
#include "static_map.h"
}

const int hm_hashtable_size_bytes = 256 * 256 * sizeof(uint32_t);
const uint16_t hm_max_hash = 65535; // 256*256-1.
const size_t alignment = 8;

#define debugf(fmt, ...)                                                       \
  do {                                                                         \
  } while (0)

// Integer comparisons in SSE and AVX are always signed. To compare unsigned
// integers the operands have to be biased by subtracting the smallest signed
// integer of the same length from them. This operation transforms the smallest
// possible unsigned integer zero to the smallest possible signed integer. Since
// singed integers are stored in two’s complement the same effect can be
// achieved by using an addition, because the smallest signed integer is its own
// negation. Yet a different option is to use the XOR operation, in other words
// a carry-less addition. It behaves identically to a regular addition since a
// carry can only be generated in the most significant bit. A different view is
// to see that all these operations toggle the most significant bit, which is
// set for negative numbers in two’s complement. [Gie16]
const uint32_t ip_xor = 1 << 31;

typedef struct hm_sm_database {
  size_t list_size;
  uint32_t *hashtable;
  int32_t *max_ips;
  uint64_t *values;
} hm_sm_database_t;

struct hm_input_elem {
  uint32_t ip;
  uint8_t cidr_prefix;
  uint64_t value;
};

struct hm_elem {
  uint32_t ip;
  uint64_t value;
};

static inline uint32_t hm_end_ip_of_zone(uint32_t ip, uint8_t cidr_prefix) {
  uint32_t zero_bits = 32 - cidr_prefix;
  return (((ip) >> zero_bits) + 1) << zero_bits;
}

// Round up to even number.
static inline size_t hm_aligned_size(size_t list_size) {
  return list_size + (list_size & 1);
}

static inline char *align8(char *addr) {
  return (char *)(((uintptr_t)(addr) & ~(alignment - 1)) + alignment);
}

static inline size_t list_size_to_db_place_size(size_t list_size) {
  return sizeof(hm_sm_database_t) + hm_hashtable_size_bytes +
         hm_aligned_size(list_size) * sizeof(uint32_t) +
         list_size * sizeof(uint64_t);
}

static inline size_t list_size_to_serialized_size(size_t list_size) {
  // Serialized form:
  // uint64_t list_size
  // list_size * uint32_t elements - max_ips.
  // list_size * uint64_t elements - values.
  // The order of bytes is host.
  return 8 + list_size * (4 + 8);
}

static inline void fill_hashtable(hm_sm_database_t *db) {
  int32_t *db_ips_end = db->max_ips + db->list_size;
  for (uint32_t hash = 0; hash <= hm_max_hash; hash++) {
    uint32_t ip = hash << 16;
    const int32_t *it =
        std::lower_bound(db->max_ips, db_ips_end, int32_t(ip ^ ip_xor));
    size_t index = it - db->max_ips;
    db->hashtable[hash] = index;
  }
}

extern "C" HM_PUBLIC_API size_t HM_CDECL
hm_sm_db_place_size(unsigned int elements) {
  // +1 for 0.0.0.0 and +1 for 255.255.255.255.
  size_t max_sorted_size = elements * 2 + 2;
  return list_size_to_db_place_size(max_sorted_size) + alignment;
}

extern "C" HM_PUBLIC_API hm_error_t HM_CDECL
hm_sm_compile(char *db_place, size_t db_place_size, hm_sm_database_t **db_ptr,
              const uint32_t *ips, const uint8_t *cidr_prefixes,
              const uint64_t *values, unsigned int elements) {
  if (elements == 0) {
    return HM_ERROR_NO_MASKS;
  }

  // Align db_place forward, if needed.
  {
    char *db_place2 = align8(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < sizeof(hm_sm_database_t)) {
    return HM_ERROR_SMALL_PLACE;
  }

  std::vector<hm_input_elem> inputs;
  inputs.reserve(elements);
  for (int i = 0; i < elements; i++) {
    if (values[i] == HM_NO_VALUE) {
      return HM_ERROR_BAD_VALUE;
    }

    if (ips[i] & ((1 << (32 - cidr_prefixes[i])) - 1)) {
      return HM_ERROR_BAD_RANGE;
    }

    if (cidr_prefixes[i] == 0 || cidr_prefixes[i] > 32) {
      return HM_ERROR_BAD_RANGE;
    }

    hm_input_elem elem{
        .ip = ips[i],
        .cidr_prefix = cidr_prefixes[i],
        .value = values[i],
    };
    inputs.push_back(elem);
  }

  std::sort(inputs.begin(), inputs.end(), [](hm_input_elem a, hm_input_elem b) {
    if (a.ip == b.ip) {
      // This is very important to put larger networks zones before
      // smaller zones, to maintain the invariant that if zone A opens
      // before zone B, it closes at the same IP or after B closes.
      return a.cidr_prefix < b.cidr_prefix;
    }
    return a.ip < b.ip;
  });

  std::vector<hm_elem> sorted;
  std::vector<hm_elem> ends_stack;

  auto pushToSorted = [&sorted](uint32_t ip, uint64_t value) {
    if (!sorted.empty() && sorted.back().ip == ip) {
      sorted.back().value = value;
    } else {
      hm_elem elem{
          .ip = ip,
          .value = value,
      };
      sorted.push_back(elem);
    }
  };

  pushToSorted(0, HM_NO_VALUE);

  for (hm_input_elem input : inputs) {
    debugf("\ninput.ip=%x input.cidr_prefix=%d input.value=%d\n", input.ip,
           input.cidr_prefix, input.value);
    while (!ends_stack.empty() && ends_stack.back().ip < input.ip) {
      // Some zone stops before this zone starts.
      uint32_t end_ip = ends_stack.back().ip;
      debugf("ends_stack.pop_back() (ends_stack.back().ip %x < input.ip %x)\n",
             end_ip, input.ip);
      ends_stack.pop_back();
      uint64_t reopened_value = HM_NO_VALUE;
      if (!ends_stack.empty()) {
        reopened_value = ends_stack.back().value;
      }
      pushToSorted(end_ip, reopened_value);
      debugf("sorted.push_back reopened ip=%x input.value=%d\n", end_ip,
             reopened_value);
    }

    // Here it is "if", not "while", because if two zones end at the same point,
    // we keep only one entry in ends_stack. See below.
    if (!ends_stack.empty() && ends_stack.back().ip == input.ip) {
      // Some zone stops exactly where this zone starts. The zone stop is not
      // important.
      debugf("ends_stack.pop_back() (ends_stack.back().ip == input.ip = %x)\n",
             input.ip);
      ends_stack.pop_back();
    }

    pushToSorted(input.ip, input.value);
    debugf("sorted.push_back elem ip=%x input.value=%d\n", input.ip,
           input.value);

    uint32_t end_ip = hm_end_ip_of_zone(input.ip, input.cidr_prefix);

    if (!ends_stack.empty() && ends_stack.back().ip == end_ip) {
      // There is already some zone ending at the same ip. Since a smaller zone
      // has a priority over a larger zone, we redefine the value in the stack.
      ends_stack.back().value = input.value;
      debugf("ends_stack.back().value = %d (ends_stack.back().ip=end_ip=%x)\n",
             input.value, ends_stack.back().ip);
    } else {
      if (!ends_stack.empty()) {
        if (ends_stack.back().ip <= end_ip) {
          debugf("ends_stack.back().ip=%x end_ip=%x\n", ends_stack.back().ip,
                 end_ip);
        }
        assert(ends_stack.back().ip > end_ip);
      }
      // Add new element to the stack.
      hm_elem elem{
          .ip = end_ip,
          .value = input.value,
      };
      ends_stack.push_back(elem);
      debugf("ends_stack.push_back ip=%x value=%d\n", end_ip, input.value);
    }
  }

  // Close ending zones. Since zones can be nested, they can produce
  // non-empty values (all but last).
  while (!ends_stack.empty()) {
    // Some zone stops before this zone starts.
    uint32_t end_ip = ends_stack.back().ip;
    debugf("ends_stack.pop_back() (ends_stack.back().ip %x < input.ip %x)\n",
           end_ip, input.ip);
    ends_stack.pop_back();
    uint64_t reopened_value = HM_NO_VALUE;
    if (!ends_stack.empty()) {
      reopened_value = ends_stack.back().value;
    }
    pushToSorted(end_ip, reopened_value);
    debugf("sorted.push_back reopened ip=%x input.value=%d\n", end_ip,
           reopened_value);
  }

  // Append an element larger than largest possible IP.
  // There is a check in the beginning of hm_sm_find that IP <= 255.0.0.0.
  {
    uint32_t ip = 0x00000000;
    uint64_t value = HM_NO_VALUE;
    // After -1 below it becomes the largest value.
    pushToSorted(ip, value);
    debugf("sorted.push_back final2 ip=%x input.value=%d\n", ip, value);
  }

  // Shift all IPs to compare as signed integers.
  for (int i = 0; i < sorted.size(); i++) {
    sorted[i].ip ^= ip_xor;
  }

  hm_sm_database_t *db = reinterpret_cast<hm_sm_database_t *>(db_place);
  *db_ptr = db;
  db->list_size = sorted.size() - 1;
  db_place += sizeof(hm_sm_database_t);

  if (db_place_size < list_size_to_db_place_size(db->list_size)) {
    return HM_ERROR_SMALL_PLACE;
  }

  db->hashtable = reinterpret_cast<uint32_t *>(db_place);
  db_place += hm_hashtable_size_bytes;

  db->max_ips = reinterpret_cast<int32_t *>(db_place);
  db_place += hm_aligned_size(sorted.size() - 1) * sizeof(uint32_t);

  db->values = reinterpret_cast<uint64_t *>(db_place);

  for (int i = 0; i < sorted.size() - 1; i++) {
    db->max_ips[i] = sorted[i + 1].ip - 1;
    db->values[i] = sorted[i].value;
  }

  fill_hashtable(db);

  return HM_SUCCESS;
}

extern "C" HM_PUBLIC_API uint64_t HM_CDECL
hm_sm_find(const hm_sm_database_t *db, const uint32_t ip0) {
  // Use the hash table to find /16 place in the sorted list and use binary
  // search inside it.
  uint32_t begin = db->hashtable[ip0 >> 16];

  int32_t ip = ip0 ^ ip_xor;

  const int32_t *it = db->max_ips + begin;

  // Find the first IP in the list greater than the provided IP.
  while (*it < ip) {
    it++;
  }

  size_t index = it - db->max_ips;

  return db->values[index];
}

extern "C" HM_PUBLIC_API size_t HM_CDECL
hm_sm_serialized_size(const hm_sm_database_t *db) {
  return list_size_to_serialized_size(db->list_size);
}

extern "C" HM_PUBLIC_API hm_error_t HM_CDECL
hm_sm_serialize(char *buffer, size_t buffer_size, const hm_sm_database_t *db) {
  if (buffer_size < hm_sm_serialized_size(db)) {
    return HM_ERROR_SMALL_PLACE;
  }

  uint64_t *list_size = reinterpret_cast<uint64_t *>(buffer);
  *list_size = db->list_size;
  buffer += sizeof(uint64_t);

  uint32_t *max_ips = reinterpret_cast<uint32_t *>(buffer);
  for (int i = 0; i < db->list_size; i++) {
    max_ips[i] = db->max_ips[i];
  }
  buffer += sizeof(uint32_t) * db->list_size;

  uint64_t *values = reinterpret_cast<uint64_t *>(buffer);
  for (int i = 0; i < db->list_size; i++) {
    values[i] = db->values[i];
  }

  return HM_SUCCESS;
}

extern "C" HM_PUBLIC_API hm_error_t HM_CDECL
hm_sm_db_place_size_from_serialized(size_t *db_place_size, const char *buffer,
                                    size_t buffer_size) {
  if (buffer_size < sizeof(uint64_t)) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t *list_size = reinterpret_cast<const uint64_t *>(buffer);
  if (buffer_size < list_size_to_serialized_size(*list_size)) {
    return HM_ERROR_SMALL_PLACE;
  }

  if (*list_size == 0) {
    return HM_ERROR_NO_MASKS;
  }

  *db_place_size = list_size_to_db_place_size(*list_size) + alignment;

  return HM_SUCCESS;
}

extern "C" HM_PUBLIC_API hm_error_t HM_CDECL hm_sm_deserialize(
    char *db_place, size_t db_place_size, hm_sm_database_t **db_ptr,
    const char *buffer, size_t buffer_size) {
  if (buffer_size < sizeof(uint64_t)) {
    return HM_ERROR_SMALL_PLACE;
  }

  const uint64_t *list_size = reinterpret_cast<const uint64_t *>(buffer);
  if (buffer_size < list_size_to_serialized_size(*list_size)) {
    return HM_ERROR_SMALL_PLACE;
  }

  if (*list_size == 0) {
    return HM_ERROR_NO_MASKS;
  }

  // Align db_place forward, if needed.
  {
    char *db_place2 = align8(db_place);
    db_place_size -= (db_place2 - db_place);
    db_place = db_place2;
  }

  if (db_place_size < list_size_to_db_place_size(*list_size)) {
    return HM_ERROR_SMALL_PLACE;
  }

  // Locate max_ips and values in the db_place.
  hm_sm_database_t *db = reinterpret_cast<hm_sm_database_t *>(db_place);
  *db_ptr = db;
  db->list_size = *list_size;
  db_place += sizeof(hm_sm_database_t);

  db->hashtable = reinterpret_cast<uint32_t *>(db_place);
  db_place += hm_hashtable_size_bytes;

  db->max_ips = reinterpret_cast<int32_t *>(db_place);
  db_place += hm_aligned_size(*list_size) * sizeof(uint32_t);

  db->values = reinterpret_cast<uint64_t *>(db_place);

  // Now locate max_ips and values in the buffer.
  buffer += sizeof(uint64_t);
  const uint32_t *max_ips = reinterpret_cast<const uint32_t *>(buffer);
  buffer += sizeof(uint32_t) * (*list_size);
  const uint64_t *values = reinterpret_cast<const uint64_t *>(buffer);

  // Copy the values.
  for (int i = 0; i < *list_size; i++) {
    db->max_ips[i] = max_ips[i];
  }
  for (int i = 0; i < *list_size; i++) {
    db->values[i] = values[i];
  }

  fill_hashtable(db);

  return HM_SUCCESS;
}
