/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {},
    screens: { // 响应式断点，适配移动端/平板/PC
      'sm': '320px',
      'md': '768px',
      'lg': '1024px',
    }
  },
  plugins: [],
}