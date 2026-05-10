/* eslint-disable @typescript-eslint/no-explicit-any */
import { message } from "antd";
import { httpRequest } from "../utils";

export interface ToastConfig {
  config_type: "toast";
  url: string;
  method: string;
  data: Record<string, any> | object | undefined;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export async function renderToast(toastConfig: ToastConfig, params: any) {
  params = params || {}
  const config = toastConfig.data || {}
  params = {...params, ...config}
  const res = await httpRequest(toastConfig.method, toastConfig.url, params);
  message.success("操作成功：" + JSON.stringify(res));
}
