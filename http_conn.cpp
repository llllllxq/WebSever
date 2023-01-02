#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/lxq/WebSever/resources";

//变量初始化
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void setNonblocking(int fd) {
    int _fd = fcntl(fd,F_GETFD);
    _fd |= O_NONBLOCK;
    fcntl(fd, F_SETFL, _fd);
} 

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    //设置文件描述符为非阻塞 :ET模式下  一次性读完会阻塞
    setNonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改文件描述符，重置socket上的EPOLLONESHORT事件
//以确保下一次可读时，EPOLLIN事件能被触发
//(同一个socket，这一次到达的数据被一个线程处理完了 重新注册事件，下一次到达的数据可以被另一个线程处理，但是不能多个线程同时处理同一批数据)
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::close_conn() {
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count --;
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    //端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count ++;

    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linker = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    
    bytes_to_send = 0;
    bytes_have_sent = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_real_file,FILENAME_LEN);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
}

//循环读取客户数据，直到无数据可读 或者 对方关闭连接
bool http_conn::read() {
    if (m_read_idx > READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv (m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);

        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {//当应用程序在socket中设置O_NONBLOCK属性后，如果缓存被占满，recv就会返回EAGAIN或EWOULDBLOCK 的错误
                break;
            }
            return false;
        } else if (bytes_read == 0) {//对方关闭连接
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
}

http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++ m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx ++] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx ++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // GET / HTTP/1.1
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    // GET\0/HTTP/1.1
    *m_url ++ = '\0';
    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version ++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
} 

http_conn::HTTP_CODE http_conn::parse_request_headers(char* text) {
    //遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        //如果有消息体
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;//否则说明我们已经得到了一个完整的HTTP请求
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        //处理Connection头部字段   Connection: keep-alive
        text += 11;
        text += strspn(text, " \t"); //可能存在空格
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linker = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop!unknow header %s\n", text);
    }
    return NO_REQUEST;
}

//我们并没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_request_content(char* text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
    || ((line_status = parse_line()) == LINE_OK)) {//到消息体之后不用解析行了，用不到后面的那个条件了
        text = get_line();
        m_start_line = m_checked_idx;
        printf("got 1 http line:%s\n",text);

        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE : 
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;

            case CHECK_STATE_HEADER : 
                ret = parse_request_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;

            case CHECK_STATE_CONTENT : 
                ret = parse_request_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;

            default : 
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
    // "/home/lxq/WebSever/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //获取文件状态信息
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    //判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    //判断是否目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    //以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射
    m_file_address = (char*) mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写HTTP响应（process_write是将输出内容准备在缓冲区， 这里是要将缓冲区内容写出去发送出去）
bool http_conn::write() {
    int temp = 0;
    if (bytes_to_send == 0) {
        //要发送的字节为0，这一次的响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while(1) {
        //分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_sent += temp;
        bytes_to_send -= temp;

        if (bytes_have_sent >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_sent - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_sent;
            m_iv[0].iov_len -= temp;
        }

        if (bytes_to_send <= 0) {
            //没有数据要发送了
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linker) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}


//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char* format, ...) { 
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format); //这是写不是读！！
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

//状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//响应头
bool http_conn::add_headers(int content_length) {  
    add_content_length(content_length);
    add_content_type();
    add_linker();
    add_blank_line();
}

bool http_conn::add_content_length(int content_length) {
    return add_response("Content-Length:%d\r\n", content_length);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linker() {
    return add_response("Connection:%s\r\n",m_linker == true ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s","\r\n");
}

//响应文
bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(http_conn::HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
          add_status_line(500, error_500_title);
          add_headers(strlen(error_500_form));
          if (!add_content(error_500_form)) {
            return false;
          }
          break;

        case BAD_REQUEST:
          add_status_line(400, error_400_title);
          add_headers(strlen(error_400_form));
          if (!add_content(error_400_form)) {
            return false;
          }
          break;

        case NO_RESOURCE:
          add_status_line(404, error_404_title);
          add_headers(strlen(error_404_form));
          if (!add_content(error_404_form)) {
            return false;
          }
          break;

        case FORBIDDEN_REQUEST:
          add_status_line(403, error_403_title);
          add_headers(strlen(error_403_form));
          if (!add_content(error_403_form)) {
            return false;
          }
          break;

        case FILE_REQUEST:
          add_status_line(200, ok_200_title);
          add_headers(m_file_stat.st_size);
          m_iv[0].iov_base = m_write_buf;
          m_iv[0].iov_len = m_write_idx;
          m_iv[1].iov_base = m_file_address;
          m_iv[1].iov_len = m_file_stat.st_size;
          m_iv_count = 2;
          bytes_to_send = m_write_idx + m_file_stat.st_size;
          return true;
        
        default:
          return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process() {
    //解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {//没读完
        modfd(m_epollfd, m_sockfd, EPOLLIN);  //注册事件  "通知" 继续读
        return;
    }
   
    //生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) { //写的时候出了问题，不发了 关闭连接
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT); //注册事件 "通知" 可以写了
}