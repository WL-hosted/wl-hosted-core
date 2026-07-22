#include "common.pb.h"
#include "sim_sideband.pb.h"

#include "pb_decode.h"
#include "pb_encode.h"

#include <stdint.h>

int main(void) {
    uint8_t buffer[64];
    wlh_protocol_v1_Version version = wlh_protocol_v1_Version_init_zero;
    wlh_protocol_v1_Version decoded_version = wlh_protocol_v1_Version_init_zero;
    wlh_sim_v1_SimFaultResponse response =
        wlh_sim_v1_SimFaultResponse_init_zero;
    wlh_sim_v1_SimFaultResponse decoded_response =
        wlh_sim_v1_SimFaultResponse_init_zero;
    pb_ostream_t output;
    pb_istream_t input;

    version.major = 1;
    output = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&output, wlh_protocol_v1_Version_fields, &version)) {
        return 1;
    }
    input = pb_istream_from_buffer(buffer, output.bytes_written);
    if (!pb_decode(&input, wlh_protocol_v1_Version_fields, &decoded_version) ||
        decoded_version.major != 1) {
        return 2;
    }

    response.request_id = 1;
    response.accepted = true;
    output = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&output, wlh_sim_v1_SimFaultResponse_fields, &response)) {
        return 3;
    }
    input = pb_istream_from_buffer(buffer, output.bytes_written);
    if (!pb_decode(
            &input, wlh_sim_v1_SimFaultResponse_fields, &decoded_response
        ) ||
        decoded_response.request_id != 1 || !decoded_response.accepted) {
        return 4;
    }

    return 0;
}
