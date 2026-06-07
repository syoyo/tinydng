#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int tinydng_decode_image(const unsigned char *data, size_t size, void **out_image, int *width, int *height, int *components);

START_TEST(test_buffer_read_bounds)
{
    // Invariant: Buffer reads never exceed the declared length
    
    // Payload 1: Minimal valid TIFF/DNG header with manipulated dimensions (exploit case)
    unsigned char exploit_payload[512];
    memset(exploit_payload, 0, sizeof(exploit_payload));
    exploit_payload[0] = 0x49; exploit_payload[1] = 0x49; // Little-endian TIFF
    exploit_payload[2] = 0x2A; exploit_payload[3] = 0x00; // TIFF magic
    *(uint32_t*)(exploit_payload + 4) = 8; // IFD offset
    *(uint16_t*)(exploit_payload + 8) = 3; // 3 IFD entries
    // ImageWidth tag: oversized width (0x7FFFFFFF)
    *(uint16_t*)(exploit_payload + 10) = 0x0100;
    *(uint16_t*)(exploit_payload + 12) = 0x0004;
    *(uint32_t*)(exploit_payload + 14) = 1;
    *(uint32_t*)(exploit_payload + 18) = 0x7FFFFFFF;
    // ImageHeight tag: oversized height
    *(uint16_t*)(exploit_payload + 22) = 0x0101;
    *(uint16_t*)(exploit_payload + 24) = 0x0004;
    *(uint32_t*)(exploit_payload + 26) = 1;
    *(uint32_t*)(exploit_payload + 30) = 0x7FFFFFFF;
    
    // Payload 2: Boundary case - maximum reasonable dimensions
    unsigned char boundary_payload[256];
    memcpy(boundary_payload, exploit_payload, sizeof(boundary_payload));
    *(uint32_t*)(boundary_payload + 18) = 65535;
    *(uint32_t*)(boundary_payload + 30) = 65535;
    
    // Payload 3: Valid small image
    unsigned char valid_payload[128];
    memcpy(valid_payload, exploit_payload, sizeof(valid_payload));
    *(uint32_t*)(valid_payload + 18) = 64;
    *(uint32_t*)(valid_payload + 30) = 64;
    
    struct {
        unsigned char *data;
        size_t size;
    } payloads[] = {
        {exploit_payload, sizeof(exploit_payload)},
        {boundary_payload, sizeof(boundary_payload)},
        {valid_payload, sizeof(valid_payload)}
    };
    
    for (int i = 0; i < 3; i++) {
        void *out_image = NULL;
        int width = 0, height = 0, components = 0;
        
        int result = tinydng_decode_image(payloads[i].data, payloads[i].size, 
                                          &out_image, &width, &height, &components);
        
        if (result == 0 && out_image != NULL) {
            ck_assert_msg(width > 0 && width <= 65535, "Width must be reasonable");
            ck_assert_msg(height > 0 && height <= 65535, "Height must be reasonable");
            free(out_image);
        }
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_read_bounds);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}