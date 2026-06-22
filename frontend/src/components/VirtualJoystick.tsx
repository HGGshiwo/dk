import React, {
  useState,
  useRef,
  useEffect,
  type TouchEvent,
  type MouseEvent,
  type CSSProperties,
} from "react";
import { useAppStore } from "../store/useAppStore";
import { httpRequest } from "../utils";

// 摇杆移动时的回调参数类型
export interface JoystickMoveData {
  x: number; // 水平偏移比例 -1(左) ~ 1(右)
  y: number; // 垂直偏移比例 -1(下) ~ 1(上)
  isDragging: boolean; // 是否正在拖拽摇杆
}

// 摇杆组件Props类型
export interface VirtualJoystickProps {
  /** 摇杆移动回调（实时触发） */
  onMove: (data: JoystickMoveData) => void;
  /** 摇杆整体尺寸（底座直径，单位px），默认120 */
  size?: number;
  /** 摇杆核心直径（单位px），默认40 */
  dotSize?: number;
  /** 底座背景色，默认rgba(0,0,0,0.3) */
  baseColor?: string;
  /** 摇杆核心背景色，默认#fff */
  dotColor?: string;
  /** 摇杆固定位置，默认右下角 { right: 20, bottom: 20 } */
  position?: {
    top?: number;
    right?: number;
    bottom?: number;
    left?: number;
  };
  /** 自定义样式 */
  style?: CSSProperties;
}

// 默认配置
const defaultProps: Omit<VirtualJoystickProps, "onMove"> = {
  size: 120,
  dotSize: 40,
  baseColor: "rgba(0, 0, 0, 0.3)",
  dotColor: "#ffffff",
  style: {},
};

const VirtualJoystick: React.FC<VirtualJoystickProps> = (props) => {
  const { onMove, size, dotSize, baseColor, dotColor, position, style } = {
    ...defaultProps,
    ...props,
  };

  // 摇杆容器Ref
  const joystickRef = useRef<HTMLDivElement>(null);
  // 拖拽状态：是否正在拖拽、摇杆核心的偏移坐标（相对中心）
  const [isDragging, setIsDragging] = useState(false);
  const [dotOffset, setDotOffset] = useState({ x: 0, y: 0 });

  // 计算核心常量
  const baseRadius = size! / 2; // 底座半径
  const dotRadius = dotSize! / 2; // 核心半径
  const maxMoveDistance = baseRadius - dotRadius; // 核心最大可移动距离（防止超出底座）

  // 通用：获取触摸/鼠标点相对于摇杆中心的偏移坐标
  const getRelativeOffset = (clientX: number, clientY: number) => {
    if (!joystickRef.current) return { x: 0, y: 0 };
    const rect = joystickRef.current.getBoundingClientRect();
    // 摇杆中心的屏幕坐标
    const centerX = rect.left + baseRadius;
    const centerY = rect.top + baseRadius;
    // 计算触摸/鼠标点相对中心的偏移
    return {
      x: clientX - centerX,
      y: clientY - centerY,
    };
  };

  // 通用：限制偏移坐标在最大距离内（超出则按比例缩放，保证核心在底座内）
  const clampOffset = (x: number, y: number) => {
    // 计算当前偏移的距离（勾股定理）
    const distance = Math.sqrt(x ** 2 + y ** 2);
    // 未超出最大距离，直接返回；超出则按比例缩放
    if (distance <= maxMoveDistance) return { x, y };
    return {
      x: (x / distance) * maxMoveDistance,
      y: (y / distance) * maxMoveDistance,
    };
  };

  // 通用：触发移动回调（转换为-1~1的归一化比例）
  const triggerMoveCallback = (x: number, y: number, dragging: boolean) => {
    onMove({
      x: maxMoveDistance === 0 ? 0 : x / maxMoveDistance,
      y: maxMoveDistance === 0 ? 0 : -y / maxMoveDistance, // y的正方向是反的
      isDragging: dragging,
    });
  };

  // 开始拖拽（鼠标按下/触摸开始）
  const handleStart = (
    e: MouseEvent<HTMLDivElement> | TouchEvent<HTMLDivElement>,
  ) => {
    // 鼠标事件：只响应左键（button=0），阻止右键触发
    if ("button" in e && e.button !== 0) return;
    // 触摸事件：取第一个触摸点（防止多点触摸干扰）
    const point = "touches" in e ? e.touches[0] : e;
    const offset = getRelativeOffset(point.clientX, point.clientY);
    const clamped = clampOffset(offset.x, offset.y);

    setIsDragging(true);
    setDotOffset(clamped);
    triggerMoveCallback(clamped.x, clamped.y, true);

    // 阻止事件冒泡/默认行为（防止移动端滚动、PC选中文本等）
    e.stopPropagation();
    e.preventDefault();
  };

  // 拖拽中（鼠标移动/触摸移动）
  const handleDrag = <T extends globalThis.MouseEvent | globalThis.TouchEvent>(e: T) => {
    if (!isDragging || !joystickRef.current) return;

    const point: globalThis.Touch | globalThis.MouseEvent = "touches" in e ? e.touches[0] : e;
    const offset = getRelativeOffset(point.clientX, point.clientY);
    const clamped = clampOffset(offset.x, offset.y);

    setDotOffset(clamped);
    triggerMoveCallback(clamped.x, clamped.y, true);

    e.stopPropagation();
    e.preventDefault();
  };

  // 结束拖拽（鼠标松开/触摸结束/触摸离开）
  const handleEnd = () => {
    if (!isDragging) return;
    setIsDragging(false);
    setDotOffset({ x: 0, y: 0 });
    triggerMoveCallback(0, 0, false); // 归位后触发一次回调（x/y为0）
  };

  // 绑定/解绑全局事件（拖拽时需要监听document的移动/松开，否则移出摇杆区域会失效）
  useEffect(() => {
    if (isDragging) {
      // 绑定全局事件
      document.addEventListener("mousemove", handleDrag<globalThis.MouseEvent>);
      document.addEventListener("mouseup", handleEnd);
      document.addEventListener("touchmove", handleDrag<globalThis.TouchEvent>, { passive: false }); // passive: false 才能阻止默认行为
      document.addEventListener("touchend", handleEnd);
      document.addEventListener("touchcancel", handleEnd); // 处理触摸意外中断（如弹窗、滑动）
    }

    // 清理：组件卸载/拖拽结束时解绑事件，防止内存泄漏
    return () => {
      document.removeEventListener("mousemove", handleDrag);
      document.removeEventListener("mouseup", handleEnd);
      document.removeEventListener("touchmove", handleDrag);
      document.removeEventListener("touchend", handleEnd);
      document.removeEventListener("touchcancel", handleEnd);
    };
  }, [isDragging]);

  // 摇杆容器样式
  const containerStyle: CSSProperties = {
    display: "flex",
    alignItems: "center",
    justifyContent: "center",
    width: size,
    height: size,
    ...position,
    ...style,
  };

  // 摇杆底座样式
  const baseStyle: CSSProperties = {
    width: "100%",
    height: "100%",
    borderRadius: "50%",
    backgroundColor: baseColor,
    display: "flex",
    alignItems: "center",
    justifyContent: "center",
    touchAction: "none", // 阻止浏览器默认的触摸行为（如缩放、滚动）
    userSelect: "none", // 阻止PC端选中文本
  };

  // 摇杆核心样式（根据偏移量做平移）
  const dotStyle: CSSProperties = {
    width: dotSize,
    height: dotSize,
    borderRadius: "50%",
    backgroundColor: dotColor,
    boxShadow: "0 2px 8px rgba(0,0,0,0.4)",
    cursor: "grab",
    position: "absolute",
    transform: `translate(${dotOffset.x}px, ${dotOffset.y}px)`,
    transition: isDragging ? "none" : "transform 0.2s ease-out", // 归位时加缓动动画
    touchAction: "none",
    userSelect: "none",
  };

  return (
    <div
      ref={joystickRef}
      style={containerStyle}
      data-testid="virtual-joystick"
    >
      <div
        style={baseStyle}
        onMouseDown={handleStart}
        onTouchStart={handleStart}
      >
        <div style={dotStyle} />
      </div>
    </div>
  );
};

function VirtualJoystickGroup() {
  const config = useAppStore(state => state.config);
  const joystickConfig = config?.joystick || [];
  const containerStyle: CSSProperties = {
    zIndex: 9999,
    position: "fixed",
    right: 20,
    bottom: 20,
  };
  return (
    <div style={containerStyle}>
      {joystickConfig.map(item => {
        return (
          <VirtualJoystick
            key={item.key}
            onMove={(data) => httpRequest(item["method"], item["url"], data)}
          />
        );
      })}
    </div>
  );
}

export { VirtualJoystickGroup, VirtualJoystick };
