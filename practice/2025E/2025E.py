# LDD_ROG 2026.7.9
# 矩形效果很离谱的话，优先检查前面几个参数、二值化反向问题、矩形跳变过滤阈值
# 浮点运算很吃性能！！！



# 视觉库
from maix import app, camera, display, image, time  
import cv2                                          
import numpy as np                                  
# 串口库
from maix import pinmap, err
from maix.comm import modbus                  



CAM_W = 512    # 定义画面宽
CAM_H = 320    # 定义画面高
min_rects_area = 3600          # 过滤低于该面积的矩形框
APPROX_EPSILON_RATIO = 0.02    # 该参数配合cv2.approxPolyDP()使用，以轮廓周长6%的长度作为滑动窗口大小，处于该窗口内的细微抖动直接抹去
MIN_ASPECT = 0.5               # 过滤的最小宽高比
MAX_ASPECT = 2.8               # 过滤的最大宽高比

cx_before = 256                # 初始化前一帧矩形中心横坐标为画面中心
cy_before = 160                # 初始化前一帧矩形中心纵坐标为画面中心
last_area = None               # 初始化最后一帧获取的矩形面积，用于面积连续性过滤防止跳变



# ========== Modbus 配置 ==========
MODBUS_BAUD = 115200           # 波特率，和 MCU 端一致
MODBUS_SLAVE_ID = 1            # MaixCAM 的从机地址
# 寄存器地址分配（这些地址要和 MCU 端约定一致）
REG_ERR_X = 0x0000             # err_x 存到保持寄存器 0x0000
REG_ERR_Y = 0x0001             # err_y 存到保持寄存器 0x0001
REG_RECT_STATUS = 0x0002       # 是否检测到矩形标志（0=无，1=有）
REG_CENTER_X = 0x0003          # 原图中心点坐标 cx（可选）
REG_CENTER_Y = 0x0004          # 原图中心点坐标 cy（可选）
HOLDING_REGS_COUNT = 10        # 保持寄存器总数量（≥5，留点余量）



def find_best_rect(img_cv):
    gray = cv2.cvtColor(img_cv, cv2.COLOR_RGB2GRAY)  # 转灰度 
    blur = cv2.GaussianBlur(gray, (5, 5), 0)         # 高斯模糊（高斯滤波）去噪处理。(5,5)为卷积核窗口大小，0为自动计算标准差
    binary = cv2.adaptiveThreshold(                  # 自适应二值化，应对光照不均问题
        blur,                         # 输入灰度图
        255,                          # 亮度最大值，255即白色
        cv2.ADAPTIVE_THRESH_MEAN_C,   # 邻域均值-常数C=自适应阈值。还可换用高斯阈值，更抗光照影响。
        cv2.THRESH_BINARY_INV,                # 反向二值化（背景变黑，前景变白）。不想要反向就去掉_INV
        31,                           # 邻域取的正方形区域分辨率大小
        8,                            # 常数C，调大则剩余前景变少，相应边缘变细
    )

    contours, _ = cv2.findContours(   # 寻找轮廓，并将所有轮廓存入contours序列
        binary,                       # 二值图像
        cv2.RETR_EXTERNAL,            # 只检测外轮廓
        cv2.CHAIN_APPROX_SIMPLE,      # 压缩轮廓点，只保留端点（节省内存）
    )

    best = None                          # 初始化最佳候选矩形
    for contour in contours:             # 遍历所有轮廓
        area = cv2.contourArea(contour)  # 计算轮廓面积
        if area < min_rects_area:        # 初次过滤：面积
            continue                     

        perimeter = cv2.arcLength(contour, True)       # 计算轮廓周长
        approx = cv2.approxPolyDP(contour, APPROX_EPSILON_RATIO * perimeter, True)  # 近似成多边形。该函数返回各角点坐标组成的数组
        if len(approx) != 4:                           # 二次过滤：边数必须为4
            continue
        
        x, y, w, h = cv2.boundingRect(approx)          # 能运行到这一步的都是4边形，找最小正立外接矩形，xywh分别为左上角坐标和宽度高度
        aspect = w/h                                   # 三次过滤：宽高比
        if aspect <= MIN_ASPECT or aspect > MAX_ASPECT:
            continue

        rect_final_candidate = {                       # 结构体存放最终候选矩形  
            "area": area,
            "rect": (x, y, w, h),
            "center": (x + (w//2), y + (h//2)),        # //是整除，像素不能为浮点
            "s": (w * h),
            "approx": approx
        }

        if best is None or area > best["area"]:        # 保存最好矩形
            best = rect_final_candidate

    return best

# 用于快速验证：读取approx的角点并显示在原图，显示的角点都是红色且没有位置标签
def draw_points(img, points):
    for point in points:
        x, y = int(point[0]), int(point[1])
        img.draw_circle(x, y, 5, image.COLOR_RED, thickness = -1)

# 角点排序，防止approx里的四个角点顺序不对导致处理不了
def order_points(points):
    pts = points.reshape(4, 2).astype("float32")
    ordered = np.zeros((4, 2), dtype = "float32")

    s = pts.sum(axis=1)
    ordered[0] = pts[np.argmin(s)]
    ordered[2] = pts[np.argmax(s)]

    diff= np.diff(pts, axis=1).reshape(4)
    ordered[1]= pts[np.argmin(diff)]
    ordered[3]= pts[np.argmax(diff)]

    return ordered

# 用于进阶处理：画排序后的角点，有颜色且有位置标签
def draw_ordered_points(img, ordered):
    labels = ["TL", "TR", "BR", "BL"]
    colors = [
    image. COLOR_RED,
    image. COLOR_GREEN,
    image. COLOR_BLUE,
    image. COLOR_YELLOW,
    ]
    for i in range(4):                
        x, y = int(ordered[i][0]), int(ordered[i][1])
        img.draw_circle(x, y, 5, colors[i], thickness=-1)
        img.draw_string(x + 6, y - 10, labels[i], colors[i])

# 画不横平竖直的贴边四边形框，后面再做透视变换
def draw_polygon(img, ordered):
    pts = ordered.astype("int32")
    for i in range(4):                                             # 0-1-2-3-4-0角点循环画边
        x1, y1 = int(pts[i][0]), int(pts[i][1])
        x2, y2 = int(pts[(i +1)% 4][0]), int(pts[(i +1) % 4][1])   # %4是为了让第四个角点连接到第一个角点
        img.draw_line(x1, y1, x2, y2, image.COLOR_GREEN, thickness = 2)

# 透视变换
def rect_perspective(img_cv, ordered):
    
    tl, tr, br, bl = ordered                     # 取排序好的四个角点坐标
    width_top = (np.linalg.norm(tr - tl))   # linalg算边的长度
    width_bottom = (np.linalg.norm(br - bl))
    height_left = (np.linalg.norm(bl - tl))
    height_right = (np.linalg.norm(br - tr))                                            
    warp_w = int(max(width_top, width_bottom))   # 取上下边中较大的一条作为拉正后宽度
    warp_h = int(max(height_left, height_right)) # 取左右边中较大的一条作为拉正后高度
    if warp_w < 20 or warp_h < 20:               # 如果目标矩形十分小就没必要变换了
        return None
    dst = np.array(
        [
            [0, 0],
            [warp_w - 1, 0],
            [warp_w - 1, warp_h - 1],
            [0, warp_h - 1],
        ],
        dtype = "float32",
    )
    matrix = cv2.getPerspectiveTransform(ordered, dst)              # 根据四个原点和四个目标点，计算变换矩阵
    warped = cv2.warpPerspective(img_cv, matrix, (warp_w, warp_h))  # 根据变换矩阵，映射原图像素
    
    return {
        "image": warped,
        "matrix": matrix,
        "size": (warp_w, warp_h),
    }


def run():

    time.sleep(10)
    # 设置 A16/A17 引脚为 UART0 功能（Modbus RTU 走 UART0）
    pinmap.set_pin_function("A16", "UART0_TX")
    pinmap.set_pin_function("A17", "UART0_RX")

    # 初始化 Modbus RTU Slave
    # 参数: mode, 设备路径, coils起址, coils数量, discrete起址, discrete数量,
    #       holding起址, holding数量, input起址, input数量, 波特率, 从机地址
    slave = modbus.Slave(
        modbus.Mode.RTU,          # RTU 模式
        "/dev/ttyS0",             # UART0 设备
        0, 0,                     # 不用 coils
        0, 0,                     # 不用 discrete inputs
        0, HOLDING_REGS_COUNT,    # 保持寄存器：起始地址0，共10个
        0, 0,                     # 不用 input registers
        rtu_baud=MODBUS_BAUD,
        rtu_slave=MODBUS_SLAVE_ID,
        debug=False               # 正式运行关 debug，调试时可开 True
    )
    print("Modbus RTU Slave initialized on /dev/ttyS0")


    global cx_before, cy_before         # 声明这些使用的是全局变量
    cam = camera.Camera(CAM_W, CAM_H)   # 初始化摄像头
    disp = display.Display()            # 初始化屏幕

    while not app.need_exit():          # 一次while循环为一帧。not app.need_exit是检测是否有来自maixvision或者maixcam的退出信号
        img = cam.read()                # 读取maixcam采集的Image对象数据
        img_cv = image.image2cv(img, False, False)         # 把Image对象转换成opencv要的numpy格式

        best = find_best_rect(img_cv)                      # 调用函数获取最佳矩形
        if not best:                    # 如果没检测到矩形就直接跳过当前帧
            # 通知 MCU 当前帧无矩形（状态置 0）
            slave.holding_registers([0, 0, 0, 0, 0], REG_ERR_X)
            disp.show(img)
            # 继续处理 Modbus 请求后进入下一帧
            slave.receive_and_reply(0)  # 非阻塞，有请求就回复，没有就跳过
            continue

        show_img = img                                     # 使用原图

        if best:                                           # 字典不为空
            cx, cy = best["center"]                        # 取出中心点位
            xx, yy, ww, hh = best["rect"]                  # 取出矩形坐标
            s = best["s"]                                  # 当前矩形面积（调试用）
            if abs(cx - cx_before) <= 100 and abs(cy - cy_before) <= 80:               # 四次过滤：去掉明显跳变的矩形，abs()是取绝对值
                if ww < 400:                                                           # 五次过滤：去掉太大的框
                    ordered = order_points(best["approx"])                             
                    warp_result = rect_perspective(img_cv, ordered)

                    if warp_result is not None:            # 如果透视变换成功
                        std_h, std_w = warp_result["image"].shape[:2]    # :2只取warp_result前两个值，即宽度和高度，第三个值颜色通道不管      
                        center_std = (std_w // 2, std_h // 2)

                        # 矩阵反变换，便于用原图求变换图的中心误差
                        std_center_points = np.array([[center_std]], dtype=np.float32)
                        M_inv = np.linalg.inv(warp_result["matrix"])
                        original_center_point = cv2.perspectiveTransform(std_center_points, M_inv)[0][0]
                        ox, oy = int(original_center_point[0]), int(original_center_point[1])
                        
                        screen_center = (img.width() // 2, img.height() // 2)
                        err_x = ox - screen_center[0]
                        err_y = oy - screen_center[1]
                        # ---- 更新 Modbus 保持寄存器 ----
                        slave.holding_registers([err_x, err_y, 1, cx, cy], REG_ERR_X)


                        # 绘制原图上的多边形和角点
                        ordered = order_points(best["approx"])
                        draw_polygon(show_img, ordered)
                        draw_ordered_points(show_img, ordered)
                        cx_before = cx
                        cy_before = cy

                        # 绘制原图中心
                        show_img.draw_circle(ox, oy, 4, image.COLOR_RED, thickness=-1)

                        '''
                        # 显示偏移误差（绿色）
                        show_img.draw_string(ox + 10, oy - 10, f"err({err_x:+d}, {err_y:+d})", image.COLOR_GREEN)
                        # ---- 将透视变换结果缩放到小图并显示在右上角 ----
                        warped = warp_result["image"]
                        h_w, w_w = warped.shape[:2]
                        # 缩放至宽度150，高度不超过120
                        scale = 150.0 / w_w
                        new_w = 150
                        new_h = int(h_w * scale)
                        if new_h > 120:
                            scale = 120.0 / h_w
                            new_h = 120
                            new_w = int(w_w * scale)
                        warped_resized = cv2.resize(warped, (new_w, new_h))
                        warped_img = image.cv2image(warped_resized, False, False)
                        # 显示在右上角（距离边缘5像素）
                        show_img.draw_image(show_img.width() - new_w - 5, 5, warped_img)
                        '''
        else:
            show_img.draw_string(10, 10, "no rect", image.COLOR_RED)   
            
        fps = time.fps()
        show_img.draw_string(0,0,f"{fps:.1f}",color = image.COLOR_RED)   # 显示帧率
        # ---- 响应 MCU 的 Modbus 轮询 ----
        slave.receive_and_reply(0)   # 非阻塞：有请求就回复，没请求直接继续
        disp.show(show_img)                                              # 显示图像结果 