#ifndef WLH_PROTOCOL_ENDIAN_H
#define WLH_PROTOCOL_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-width little-endian helpers. Each function checks the supplied
 * span. */
int wlh_read_u16_le(const uint8_t *input, size_t input_size, uint16_t *value);
int wlh_read_i16_le(const uint8_t *input, size_t input_size, int16_t *value);
int wlh_read_u32_le(const uint8_t *input, size_t input_size, uint32_t *value);
int wlh_write_u16_le(uint8_t *output, size_t output_size, uint16_t value);
int wlh_write_i16_le(uint8_t *output, size_t output_size, int16_t value);
int wlh_write_u32_le(uint8_t *output, size_t output_size, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
