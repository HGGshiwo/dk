import axios, { type AxiosRequestConfig } from "axios";

import { message } from "antd";

export const wsURL = import.meta.env.DEV
  ? `ws://localhost:8000/ws`
  : "ws://" + window.location.host + "/ws";

export const baseURL = import.meta.env.DEV ? `http://localhost:8000` : "/";
// 创建axios实例，统一配置
const request = axios.create({
  baseURL, // 可根据实际环境修改
  timeout: 5000,
  headers: {
    "Content-Type": "application/json;charset=utf-8",
  },
});

// 请求拦截器
request.interceptors.request.use(
  (config) => {
    // 可添加token等全局参数
    return config;
  },
  (error) => {
    message.error("请求发送失败：" + error.message);
    return Promise.reject(error);
  },
);

// 响应拦截器
request.interceptors.response.use(
  (response) => {
    // 假设后端返回格式：{  status: any, msg: string }
    if (response.data.status == undefined) { // 兼容后端直接返回数据的情况
      return response.data;
    }
    const { status, msg } = response.data;
    if (status === "success") {
      return msg;
    } else {
      // message.error("请求失败：" + msg);
      return Promise.reject(new Error(msg));
    }
  },
  (error) => {
    message.error("请求失败：" + error.message);
    return Promise.reject(error);
  },
);

// 通用HTTP请求方法
export const httpRequest = async <T>(
  method: AxiosRequestConfig["method"],
  url: string,
  data?: unknown,
): Promise<T> => {
  const config: AxiosRequestConfig = { method, url };
  if (method === "GET") {
    config.params = data;
  } else {
    config.data = data;
  }
  return await request(config);
};

// 按order排序（省略order设为999）
export const sortByOrder = <T extends { order?: number }>(
  list: T[],
): T[] => {
  return list.sort((a, b) => (a.order ?? 999) - (b.order ?? 999));
};


