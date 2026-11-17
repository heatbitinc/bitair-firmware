#include <stdio.h>
#include <time.h>

#include "esp_bootloader_desc.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_log.h"
#include "esp_flash_encrypt.h"
#include "esp_ota_ops.h"
#include "esp_secure_boot.h"

#include "device_cert.h"
#include "security_info.h"

#define KEY_BLOCK_COUNT 6

/* Values that cannot change without a reboot, read once. */
typedef struct {
    bool ready;
    bool secure_boot;
    bool flash_encryption;
    int key_purpose[KEY_BLOCK_COUNT];
    bool sb_key_revoked[3];
    bool dis_pad_jtag;
    bool dis_usb_jtag;
    bool dis_download_mode;
    bool security_download;
    int app_secure_version;
    bool bootloader_desc_ok;
    esp_bootloader_desc_t bootloader;
    char running_partition[17];
} static_info_t;

static const char *TAG = "security_info";
static static_info_t s_info;

static bool efuse_bit(const esp_efuse_desc_t *field[])
{
    return esp_efuse_read_field_bit(field);
}

static void read_static_info(void)
{
    s_info.secure_boot = esp_secure_boot_enabled();
    s_info.flash_encryption = esp_flash_encryption_enabled();
    for (int i = 0; i < KEY_BLOCK_COUNT; i++) {
        s_info.key_purpose[i] = (int)esp_efuse_get_key_purpose((esp_efuse_block_t)(EFUSE_BLK_KEY0 + i));
    }
    s_info.sb_key_revoked[0] = efuse_bit(ESP_EFUSE_SECURE_BOOT_KEY_REVOKE0);
    s_info.sb_key_revoked[1] = efuse_bit(ESP_EFUSE_SECURE_BOOT_KEY_REVOKE1);
    s_info.sb_key_revoked[2] = efuse_bit(ESP_EFUSE_SECURE_BOOT_KEY_REVOKE2);
    s_info.dis_pad_jtag = efuse_bit(ESP_EFUSE_DIS_PAD_JTAG);
    s_info.dis_usb_jtag = efuse_bit(ESP_EFUSE_DIS_USB_JTAG);
    s_info.dis_download_mode = efuse_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);
    s_info.security_download = efuse_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD);

    size_t secure_version = 0;
    if (esp_efuse_read_field_cnt(ESP_EFUSE_SECURE_VERSION, &secure_version) == ESP_OK) {
        s_info.app_secure_version = (int)secure_version;
    }

    /* Bootloaders built with ESP-IDF older than 5.2 have no description. */
    s_info.bootloader_desc_ok = esp_ota_get_bootloader_description(NULL, &s_info.bootloader) == ESP_OK;

    const esp_partition_t *running = esp_ota_get_running_partition();
    snprintf(s_info.running_partition, sizeof(s_info.running_partition), "%s",
             running != NULL ? running->label : "");
    s_info.ready = true;
}

void security_info_add_to_json(cJSON *root)
{
    if (root == NULL) {
        return;
    }
    bool first = !s_info.ready;
    if (first) {
        read_static_info();
    }

    cJSON *sec = cJSON_AddObjectToObject(root, "security");
    if (sec == NULL) {
        return;
    }

    cJSON *cert = cJSON_AddObjectToObject(sec, "device_cert");
    if (cert != NULL) {
        time_t not_after = device_cert_get_not_after();
        cJSON_AddBoolToObject(cert, "enrolled", not_after != 0);
        if (not_after != 0) {
            struct tm tm;
            char buf[24];
            gmtime_r(&not_after, &tm);
            strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            cJSON_AddStringToObject(cert, "not_after", buf);
        }
    }

    cJSON_AddBoolToObject(sec, "secure_boot", s_info.secure_boot);
    cJSON_AddBoolToObject(sec, "flash_encryption", s_info.flash_encryption);
    cJSON_AddItemToObject(sec, "key_purposes", cJSON_CreateIntArray(s_info.key_purpose, KEY_BLOCK_COUNT));
    cJSON *revoked = cJSON_AddArrayToObject(sec, "sb_key_revoked");
    for (int i = 0; revoked != NULL && i < 3; i++) {
        cJSON_AddItemToArray(revoked, cJSON_CreateBool(s_info.sb_key_revoked[i]));
    }
    cJSON_AddBoolToObject(sec, "dis_pad_jtag", s_info.dis_pad_jtag);
    cJSON_AddBoolToObject(sec, "dis_usb_jtag", s_info.dis_usb_jtag);
    cJSON_AddBoolToObject(sec, "dis_download_mode", s_info.dis_download_mode);
    cJSON_AddBoolToObject(sec, "security_download", s_info.security_download);
    cJSON_AddNumberToObject(sec, "app_secure_version", s_info.app_secure_version);
    cJSON_AddStringToObject(sec, "running_partition", s_info.running_partition);

    if (s_info.bootloader_desc_ok) {
        cJSON *bl = cJSON_AddObjectToObject(sec, "bootloader");
        if (bl != NULL) {
            cJSON_AddNumberToObject(bl, "version", s_info.bootloader.version);
            cJSON_AddStringToObject(bl, "idf", s_info.bootloader.idf_ver);
            cJSON_AddNumberToObject(bl, "secure_version", s_info.bootloader.secure_version);
        }
    } else {
        cJSON_AddNullToObject(sec, "bootloader");
    }

    if (first) {
        char *json = cJSON_PrintUnformatted(sec);
        if (json != NULL) {
            ESP_LOGI(TAG, "%s", json);
            cJSON_free(json);
        }
    }
}
