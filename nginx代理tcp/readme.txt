1 nginx 编译配置需要--with-stream
2 启动服务器 python echo.py
3 启动nginx nginx
4 用客户端发送字符串 python client.py 127.0.0.1 12345 "hello"