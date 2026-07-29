#pragma once

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include "list.h"


typedef struct
{
    char sdcard_name[80];  
}CustomSDPortNode_t;

class CustomSDPort
{
private:
    const char *TAG = "SDPort";
    const char *SdName_;
    int clk_ = 39, cmd_ = 41, d0_ = 40, d1_ = 1, d2_ = 2, d3_ = 38, width_ = 4;
    int is_SdcardInitOK = 0;
    sdmmc_card_t *sdcard_host = NULL;
    list_t *ScanListHandle = NULL;

    list_node_t *CurrentlyNode = NULL; 
    uint16_t ImgValue = 0;
public:
    CustomSDPort(const char *SdName,int clk = 39,int cmd = 41,int d0 = 40,int d1 = 1,int d2 = 2,int d3 = 38,int width = 4);
    ~CustomSDPort();

    int SDPort_WriteFile(const char *path, const void *data, size_t data_len);
    int SDPort_ReadFile(const char *path, uint8_t *buffer, size_t *outLen);
    int SDPort_ReadOffset(const char *path, void *buffer, size_t len, size_t offset);
    int SDPort_WriteOffset(const char *path, const void *data, size_t len, bool append);
    sdmmc_card_t* SDPort_GetSdMMCHost();
    void SDPort_ScanListDir(const char *path);
    list_t* SDPort_GetListHost();
    int SDPort_GetSdcardInitOK();
    // Reuses the board's original SDMMC parameters. It never formats the
    // card; callers must serialize this with all product storage I/O.
    int SDPort_Remount();
    int SDPort_GetScanListValue(); 

    void SDPort_SetCurrentlyNode(list_node_t *node);
    list_node_t* SDPort_GetCurrentlyNode(void);
    uint16_t Get_Sdcard_ImgValue(void);
};
