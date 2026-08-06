import { useEffect, useRef } from "react";
import { useAppStore } from "../../store/useAppStore";

export const useOrigin = (cb: () => void) => {
  const originSetRef = useRef(false);
  const originRef = useRef({ lat: 0, lon: 0 });

  useEffect(() => {
    const checkAndSetOrigin = () => {
      const { lat, lon, pos_enu } = useAppStore.getState().stateData;
      if (!originSetRef.current) {
        if (lat != null && lon != null && (lat as number) !== 0 && (lon as number) !== 0) {
          originRef.current = { lon: lon as number, lat: lat as number };
          originSetRef.current = true;
          cb();
          return true;
        } else if (pos_enu != null) {
          originRef.current = { lon: 0, lat: 0 };
          originSetRef.current = true;
          cb();
          return true;
        }
      }
      return originSetRef.current;
    };

    // 立即检查一次
    if (!checkAndSetOrigin()) {
      // 如果尚未设置，订阅一次
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
