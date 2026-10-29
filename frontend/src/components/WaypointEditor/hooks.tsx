import { useEffect, useRef } from "react";
import { useAppStore } from "../../store/useAppStore";

export interface OriginInfo {
  lon: number;
  lat: number;
  /** pos_enu 首次出现时记录; 未记录前 enuReady=false */
  enuX?: number;
  enuY?: number;
  enuReady: boolean;
}

// 独立记录"第一次有效的 GPS"和"第一次出现的 pos_enu", 互不等待:
//  - GPS 一到即解锁航点显示/提交换算 (cb);
//  - pos_enu 一到即补全跟随基准 (enuReady)。
// 二者到达时间差约 100ms (10Hz 增量帧), 飞机位移可忽略, 无需同帧。
// 注意不能用"当时的飞机经纬度"当 pos_enu 原点: pos_enu 的原点是 EKF
// home (解锁点), 起飞后再打开页面二者不相等, 以它做换算会把目标 GPS
// 整体平移, 导致飞机朝错误方向飞。
export const useOrigin = (cb: () => void) => {
  const originRef = useRef<OriginInfo>({
    lon: 0,
    lat: 0,
    enuReady: false,
  });

  useEffect(() => {
    const checkAndSetOrigin = () => {
      const { lat, lon, pos_enu } = useAppStore.getState().stateData;
      if (
        originRef.current.lat === 0 &&
        originRef.current.lon === 0 &&
        lat != null &&
        lon != null &&
        (lat as number) !== 0 &&
        (lon as number) !== 0
      ) {
        originRef.current = {
          ...originRef.current,
          lon: lon as number,
          lat: lat as number,
        };
        cb();
      }
      if (
        !originRef.current.enuReady &&
        pos_enu != null &&
        (pos_enu as number[]).length >= 2
      ) {
        const enu = pos_enu as number[];
        originRef.current = {
          ...originRef.current,
          enuX: enu[0],
          enuY: enu[1],
          enuReady: true,
        };
      }
      return originRef.current.lat !== 0 && originRef.current.enuReady;
    };

    // 立即检查一次
    if (!checkAndSetOrigin()) {
      // 如果尚未完整设置，订阅一次
      const unsubscribe = useAppStore.subscribe(() => {
        if (checkAndSetOrigin()) {
          unsubscribe(); // 设置成功后取消订阅
        }
      });
      return unsubscribe;
    }
  }, [cb]); // 依赖稳定，不会重新执行

  return originRef;
};
