#include "w25qx.h"
#include <stddef.h>
#include "bsp_time.h"

#define W25QX_CMD_WRITE_ENABLE   0x06U
#define W25QX_CMD_READ_STATUS1   0x05U
#define W25QX_CMD_READ_JEDEC_ID  0x9FU
#define W25QX_CMD_READ_DATA      0x03U
#define W25QX_CMD_PAGE_PROGRAM   0x02U
#define W25QX_CMD_SECTOR_ERASE4K 0x20U
#define W25QX_CMD_BLOCK_ERASE32K 0x52U
#define W25QX_CMD_BLOCK_ERASE64K 0xD8U
#define W25QX_CMD_CHIP_ERASE     0xC7U

#define W25QX_SR1_BUSY_MASK      0x01U

static void W25Qx_Select(SPIInstance *spi)
{
	DL_GPIO_clearPins(spi->GPIOx, spi->cs_pin);
	if (spi->cs_pin_state != NULL)
	{
		*spi->cs_pin_state = 0U;
	}
	spi->CS_State = 0U;
}

static void W25Qx_Deselect(SPIInstance *spi)
{
	DL_GPIO_setPins(spi->GPIOx, spi->cs_pin);
	if (spi->cs_pin_state != NULL)
	{
		*spi->cs_pin_state = 1U;
	}
	spi->CS_State = 1U;
}

static void W25Qx_FlushRxFIFO(SPI_HandleTypeDef *spi)
{
	while (!DL_SPI_isRXFIFOEmpty(spi))
	{
		(void)DL_SPI_receiveData8(spi);
	}
}

static W25QxStatus W25Qx_TransferNoCS(W25QxHandle *h, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t i;

	if ((h == NULL) || (h->spi == NULL) || (len == 0U))
	{
		return W25QX_ERR_PARAM;
	}

	for (i = 0U; i < len; i++)
	{
		uint8_t in = 0U;
		uint8_t out = (tx != NULL) ? tx[i] : 0xFFU;

		while (!DL_SPI_transmitDataCheck8(h->spi->spi_handle, out))
		{
			;
		}

		while (!DL_SPI_receiveDataCheck8(h->spi->spi_handle, &in))
		{
			;
		}

		if (rx != NULL)
		{
			rx[i] = in;
		}
	}

	while (DL_SPI_isBusy(h->spi->spi_handle))
	{
		;
	}

	return W25QX_OK;
}

static W25QxStatus W25Qx_CheckHandle(W25QxHandle *h)
{
	if ((h == NULL) || (h->spi == NULL))
	{
		return W25QX_ERR_PARAM;
	}
	return W25QX_OK;
}

static void W25Qx_SetBlockingMode(W25QxHandle *h)
{
	SPISetMode(h->spi, SPI_BLOCK_MODE);
}

static W25QxStatus W25Qx_TransRecv(W25QxHandle *h, uint8_t *rx, const uint8_t *tx, uint32_t len)
{
	if ((h == NULL) || (h->spi == NULL) || (tx == NULL) || (len == 0U))
	{
		return W25QX_ERR_PARAM;
	}

	W25Qx_SetBlockingMode(h);
	W25Qx_FlushRxFIFO(h->spi->spi_handle);
	W25Qx_Select(h->spi);
	if (W25Qx_TransferNoCS(h, tx, rx, len) != W25QX_OK)
	{
		W25Qx_Deselect(h->spi);
		return W25QX_ERR_SPI;
	}
	W25Qx_Deselect(h->spi);

	return W25QX_OK;
}

static W25QxStatus W25Qx_Transmit(W25QxHandle *h, const uint8_t *tx, uint32_t len)
{
	if ((h == NULL) || (h->spi == NULL) || (tx == NULL) || (len == 0U))
	{
		return W25QX_ERR_PARAM;
	}

	W25Qx_SetBlockingMode(h);
	W25Qx_FlushRxFIFO(h->spi->spi_handle);
	W25Qx_Select(h->spi);
	if (W25Qx_TransferNoCS(h, tx, NULL, len) != W25QX_OK)
	{
		W25Qx_Deselect(h->spi);
		return W25QX_ERR_SPI;
	}
	W25Qx_Deselect(h->spi);

	return W25QX_OK;
}

static W25QxStatus W25Qx_SendCmdAddr(W25QxHandle *h, uint8_t cmd, uint32_t addr)
{
	uint8_t frame[4];

	frame[0] = cmd;
	frame[1] = (uint8_t)(addr >> 16);
	frame[2] = (uint8_t)(addr >> 8);
	frame[3] = (uint8_t)(addr);

	return W25Qx_Transmit(h, frame, (uint8_t)sizeof(frame));
}

W25QxStatus W25Qx_Init(W25QxHandle *h, SPIInstance *spi, uint32_t timeout_ms)
{
	if ((h == NULL) || (spi == NULL))
	{
		return W25QX_ERR_PARAM;
	}

	h->spi = spi;
	h->timeout_ms = timeout_ms;
	W25Qx_SetBlockingMode(h);

	return W25QX_OK;
}

W25QxStatus W25Qx_ReadStatus1(W25QxHandle *h, uint8_t *status1)
{
	uint8_t tx[2];
	uint8_t rx[2];
	W25QxStatus st;

	if (status1 == NULL)
	{
		return W25QX_ERR_PARAM;
	}

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	tx[0] = W25QX_CMD_READ_STATUS1;
	tx[1] = 0xFFU;

	st = W25Qx_TransRecv(h, rx, tx, (uint8_t)sizeof(tx));
	if (st != W25QX_OK)
	{
		return st;
	}

	*status1 = rx[1];
	return W25QX_OK;
}

W25QxStatus W25Qx_WriteEnable(W25QxHandle *h)
{
	uint8_t cmd = W25QX_CMD_WRITE_ENABLE;
	W25QxStatus st = W25Qx_CheckHandle(h);

	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_Transmit(h, &cmd, 1U);
}

W25QxStatus W25Qx_WaitWhileBusy(W25QxHandle *h)
{
	uint8_t status;
	uint32_t start;
	uint32_t timeout_us = 0U;
	W25QxStatus st;

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	start = TimeGetTick();
	if (h->timeout_ms > 0U)
	{
		if (h->timeout_ms > (UINT32_MAX / 1000U))
		{
			timeout_us = UINT32_MAX;
		}
		else
		{
			timeout_us = h->timeout_ms * 1000U;
		}
	}

	while (1)
	{
		st = W25Qx_ReadStatus1(h, &status);
		if (st != W25QX_OK)
		{
			return st;
		}

		if ((status & W25QX_SR1_BUSY_MASK) == 0U)
		{
			return W25QX_OK;
		}

		if ((timeout_us > 0U) && ((TimeGetTick() - start) >= timeout_us))
		{
			return W25QX_ERR_TIMEOUT;
		}

		TimeDelayUs(50U);
	}
}

W25QxStatus W25Qx_ReadJedecId(W25QxHandle *h, uint8_t *manufacturer, uint8_t *memory_type,
							  uint8_t *capacity)
{
	uint8_t tx[4];
	uint8_t rx[4];
	W25QxStatus st;

	if ((manufacturer == NULL) || (memory_type == NULL) || (capacity == NULL))
	{
		return W25QX_ERR_PARAM;
	}

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	tx[0] = W25QX_CMD_READ_JEDEC_ID;
	tx[1] = 0xFFU;
	tx[2] = 0xFFU;
	tx[3] = 0xFFU;

	st = W25Qx_TransRecv(h, rx, tx, (uint8_t)sizeof(tx));
	if (st != W25QX_OK)
	{
		return st;
	}

	*manufacturer = rx[1];
	*memory_type = rx[2];
	*capacity = rx[3];
	return W25QX_OK;
}

W25QxStatus W25Qx_Read(W25QxHandle *h, uint32_t addr, uint8_t *data, uint32_t len)
{
	uint8_t header[4];
	W25QxStatus st;

	if ((data == NULL) || (len == 0U))
	{
		return W25QX_ERR_PARAM;
	}

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	header[0] = W25QX_CMD_READ_DATA;
	header[1] = (uint8_t)(addr >> 16);
	header[2] = (uint8_t)(addr >> 8);
	header[3] = (uint8_t)(addr);

	W25Qx_SetBlockingMode(h);
	W25Qx_FlushRxFIFO(h->spi->spi_handle);
	W25Qx_Select(h->spi);

	st = W25Qx_TransferNoCS(h, header, NULL, sizeof(header));
	if (st != W25QX_OK)
	{
		W25Qx_Deselect(h->spi);
		return st;
	}

	st = W25Qx_TransferNoCS(h, NULL, data, len);
	W25Qx_Deselect(h->spi);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25QX_OK;
}

W25QxStatus W25Qx_PageProgram(W25QxHandle *h, uint32_t addr, const uint8_t *data, uint16_t len)
{
	uint8_t header[4];
	uint16_t page_off;
	W25QxStatus st;

	if ((data == NULL) || (len == 0U) || (len > W25QX_PAGE_SIZE))
	{
		return W25QX_ERR_PARAM;
	}

	page_off = (uint16_t)(addr & (W25QX_PAGE_SIZE - 1U));
	if ((uint16_t)(page_off + len) > W25QX_PAGE_SIZE)
	{
		return W25QX_ERR_PARAM;
	}

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_WriteEnable(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	header[0] = W25QX_CMD_PAGE_PROGRAM;
	header[1] = (uint8_t)(addr >> 16);
	header[2] = (uint8_t)(addr >> 8);
	header[3] = (uint8_t)(addr);

	W25Qx_SetBlockingMode(h);
	W25Qx_FlushRxFIFO(h->spi->spi_handle);
	W25Qx_Select(h->spi);

	st = W25Qx_TransferNoCS(h, header, NULL, sizeof(header));
	if (st != W25QX_OK)
	{
		W25Qx_Deselect(h->spi);
		return st;
	}

	st = W25Qx_TransferNoCS(h, data, NULL, len);
	W25Qx_Deselect(h->spi);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_WaitWhileBusy(h);
}

W25QxStatus W25Qx_Write(W25QxHandle *h, uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint32_t written = 0U;
	W25QxStatus st;

	if ((data == NULL) || (len == 0U))
	{
		return W25QX_ERR_PARAM;
	}

	st = W25Qx_CheckHandle(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	while (written < len)
	{
		uint32_t cur_addr = addr + written;
		uint32_t page_off = cur_addr & (W25QX_PAGE_SIZE - 1U);
		uint32_t page_left = W25QX_PAGE_SIZE - page_off;
		uint32_t remain = len - written;
		uint16_t chunk = (uint16_t)((remain < page_left) ? remain : page_left);

		st = W25Qx_PageProgram(h, cur_addr, &data[written], chunk);
		if (st != W25QX_OK)
		{
			return st;
		}

		written += chunk;
	}

	return W25QX_OK;
}

W25QxStatus W25Qx_EraseSector4K(W25QxHandle *h, uint32_t addr)
{
	W25QxStatus st = W25Qx_CheckHandle(h);

	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_WriteEnable(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_SendCmdAddr(h, W25QX_CMD_SECTOR_ERASE4K, addr);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_WaitWhileBusy(h);
}

W25QxStatus W25Qx_EraseBlock32K(W25QxHandle *h, uint32_t addr)
{
	W25QxStatus st = W25Qx_CheckHandle(h);

	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_WriteEnable(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_SendCmdAddr(h, W25QX_CMD_BLOCK_ERASE32K, addr);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_WaitWhileBusy(h);
}

W25QxStatus W25Qx_EraseBlock64K(W25QxHandle *h, uint32_t addr)
{
	W25QxStatus st = W25Qx_CheckHandle(h);

	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_WriteEnable(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_SendCmdAddr(h, W25QX_CMD_BLOCK_ERASE64K, addr);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_WaitWhileBusy(h);
}

W25QxStatus W25Qx_ChipErase(W25QxHandle *h)
{
	uint8_t cmd = W25QX_CMD_CHIP_ERASE;
	W25QxStatus st = W25Qx_CheckHandle(h);

	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_WriteEnable(h);
	if (st != W25QX_OK)
	{
		return st;
	}

	st = W25Qx_Transmit(h, &cmd, 1U);
	if (st != W25QX_OK)
	{
		return st;
	}

	return W25Qx_WaitWhileBusy(h);
}
