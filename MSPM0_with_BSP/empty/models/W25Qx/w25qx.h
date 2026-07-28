#ifndef W25QX_H
#define W25QX_H

#include <stdbool.h>
#include <stdint.h>
#include "bsp_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define W25QX_PAGE_SIZE            256U
#define W25QX_SECTOR_SIZE          4096U
#define W25QX_BLOCK32_SIZE         32768U
#define W25QX_BLOCK64_SIZE         65536U

typedef enum
{
	W25QX_OK = 0,
	W25QX_ERR_PARAM,
	W25QX_ERR_TIMEOUT,
	W25QX_ERR_SPI,
} W25QxStatus;

typedef struct
{
	SPIInstance *spi;
	uint32_t timeout_ms;
} W25QxHandle;

W25QxStatus W25Qx_Init(W25QxHandle *h, SPIInstance *spi, uint32_t timeout_ms);
W25QxStatus W25Qx_ReadJedecId(W25QxHandle *h, uint8_t *manufacturer, uint8_t *memory_type,
							  uint8_t *capacity);

W25QxStatus W25Qx_ReadStatus1(W25QxHandle *h, uint8_t *status1);
W25QxStatus W25Qx_WriteEnable(W25QxHandle *h);
W25QxStatus W25Qx_WaitWhileBusy(W25QxHandle *h);

W25QxStatus W25Qx_Read(W25QxHandle *h, uint32_t addr, uint8_t *data, uint32_t len);
W25QxStatus W25Qx_PageProgram(W25QxHandle *h, uint32_t addr, const uint8_t *data, uint16_t len);
W25QxStatus W25Qx_Write(W25QxHandle *h, uint32_t addr, const uint8_t *data, uint32_t len);

W25QxStatus W25Qx_EraseSector4K(W25QxHandle *h, uint32_t addr);
W25QxStatus W25Qx_EraseBlock32K(W25QxHandle *h, uint32_t addr);
W25QxStatus W25Qx_EraseBlock64K(W25QxHandle *h, uint32_t addr);
W25QxStatus W25Qx_ChipErase(W25QxHandle *h);

#ifdef __cplusplus
}
#endif

#endif
