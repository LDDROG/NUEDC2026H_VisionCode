# bsp iic

> 预计增加模拟iic

## 注意事项

使用时写入地址，不需要左移！！！

DMA 模式依赖 SysConfig 已经完成 DMA 通道初始化；BSP 仅在运行时绑定地址与长度。

本 BSP 在实现层仅使用设备头里的稳定宏（例如 `I2C0/I2C1`、`I2C0_INT_IRQn/I2C1_INT_IRQn`、`DMA_I2C0_TX_TRIG` 等），不依赖 `ti_msp_dl_config.h` 的宏名。

DMA 通道号按 trigger 在运行时扫描匹配：

- I2C0 TX: `DMA_I2C0_TX_TRIG`
- I2C0 RX: `DMA_I2C0_RX_TRIG`
- I2C1 TX: `DMA_I2C1_TX_TRIG`
- I2C1 RX: `DMA_I2C1_RX_TRIG`

如果未找到匹配 trigger 的 DMA 通道，DMA 模式不会启动传输。

## 总线机制详解

关于I2C的序列传输,Restart condition和总线仲裁,请看:

https://blog.csdn.net/NeoZng/article/details/128496694

https://blog.csdn.net/NeoZng/article/details/128486366

使用序列通信则在单次通信后不会释放总线,继续占用直到调用传输函数时传入`IIC_RELEASE`参数. 这个功能只在一条总线上挂载多个主机的时候有用.
