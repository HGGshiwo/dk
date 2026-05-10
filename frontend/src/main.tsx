import { createRoot } from "react-dom/client";
import App from "./App.tsx";
import './index.css'
import 'antd/dist/reset.css'; // 引入Antd重置样式

createRoot(document.getElementById("root")!).render(<App />);
