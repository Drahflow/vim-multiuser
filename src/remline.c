/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

#include "vim.h"
#include "multiuser.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netdb.h>

#define IN_BUF_SIZE 1000000

static remote_memline_T *first_remote_memline = NULL;

/*
 * connect to a remote memline master
 */
    int
rl_connect_remote(buf, peer)
    buf_T  *buf;
    char_u *peer;
{
    char_u *host_str;
    int    host_len;
    char_u *port_str;
    int    port = -1;

    if(ml_has_remote(buf))
    {
        EMSG("buffer already connected to a master");
        goto fail;
    }

    for(port_str = peer; *port_str && *port_str != ':'; ++port_str);
    if(*port_str != ':')
    {
        EMSG("cannot parse peer address, needs <host>:<port>");
        goto fail;
    }

    host_len = port_str - peer;
    host_str = malloc(host_len + 1);
    memcpy(host_str, peer, host_len);
    host_str[host_len] = '\0';
    ++port_str;

    port = atoi(port_str);
    if(port == -1)
    {
        EMSG("cannot parse peer address, needs <host>:<port>");
        goto fail_host;
    }

    buf->b_ml.ml_remote.ml_checkpoint_cur = 0;
    buf->b_ml.ml_remote.ml_checkpoint_sent = -1;
    buf->b_ml.ml_remote.ml_checkpoint_recv = -1;
    buf->b_ml.ml_remote.ml_incoming_version = -1;
    rl_init(&buf->b_ml.ml_remote.ml_incoming);

    char *in_buf = buf->b_ml.ml_remote.in_buf = malloc(IN_BUF_SIZE);
    if(!in_buf)
        goto fail_host;

    buf->b_ml.ml_remote.in_buf_fill = buf->b_ml.ml_remote.in_buf;
    
    int s = buf->b_ml.ml_remote.ml_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(s < 0)
        goto fail_buf;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    {
        struct hostent *host_info;

	if ((host_info = gethostbyname(host_str)) == NULL)
        {
            EMSG("cannot resolve host");
            goto fail_host;
        }
	memcpy((char *)&addr.sin_addr, host_info->h_addr, host_info->h_length);
    }

    int r = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if(r < 0)
        goto fail_socket;

    buf->b_ml.ml_has_remote = 1;

    /* request full transfer to init */
    if(!rl_cmd_full_trans(buf))
        goto fail_socket;

    rl_wait(buf);

    return OK;

fail_socket:
    close(s);

fail_buf:
    free(in_buf);

fail_host:
    free(host_str);

fail:
    return FAIL;
}

/*
 * disconnect from a remote memline master
 */
    void
rl_disconnect_remote(buf)
    buf_T  *buf;
{
    if(!ml_has_remote(buf))
        return;

    close(buf->b_ml.ml_remote.ml_socket);
    buf->b_ml.ml_has_remote = 0;
}

/*
 * if sync == FALSE a non-syncing append will be sent
 * to be combined with a later delete
 */
    int
rl_append_remote(buf, lnum, sync)
    buf_T *buf;
    int   lnum;
    int   sync;
{
    char data[28];
    int  i;
    int  ret;

    ++buf->b_ml.ml_remote.ml_checkpoint_cur;

    remline_T *rl = &buf->b_ml.ml_remote.ml_incoming;
    for(i = 0; i < rl->line_count; ++i)
        if(rl->lines[i].local_lnum > lnum)
            ++rl->lines[i].local_lnum;

    char_u *line = ml_get_buf(buf, lnum + 1, FALSE);

    rl_save_uint64(data     , 28 + STRLEN(line));
    rl_save_uint32(data +  8, sync? RL_CMD_APPEND: RL_CMD_APPEND_NOSYNC);
    rl_save_uint64(data + 12, lnum);
    rl_save_uint64(data + 20, STRLEN(line));

    ret =        rl_write_all(buf->b_ml.ml_remote.ml_socket, data, 28);
    ret = ret && rl_write_all(buf->b_ml.ml_remote.ml_socket, line, STRLEN(line));

    return ret;
}

    int
rl_delete_remote(buf, lnum)
    buf_T *buf;
    int   lnum;
{
    char data[20];
    int  i;
    int  ret;

    ++buf->b_ml.ml_remote.ml_checkpoint_cur;

    remline_T *rl = &buf->b_ml.ml_remote.ml_incoming;
    for(i = 0; i < rl->line_count; ++i)
    {
        if(rl->lines[i].local_lnum == lnum)
            rl->lines[i].local_lnum = -1;
        if(rl->lines[i].local_lnum > lnum)
            --rl->lines[i].local_lnum;
    }

    rl_save_uint64(data     , 20);
    rl_save_uint32(data +  8, RL_CMD_DELETE);
    rl_save_uint64(data + 12, lnum);

    ret = rl_write_all(buf->b_ml.ml_remote.ml_socket, data, 20);

    return ret;
}

    int
rl_cmd_full_trans(buf)
    buf_T *buf;
{
    char data[12];

    ++buf->b_ml.ml_remote.ml_checkpoint_cur;

    rl_save_uint64(data, 12);
    rl_save_uint32(data + 8, RL_CMD_REQ_FULL_TRANS);

    return rl_write_all(buf->b_ml.ml_remote.ml_socket, data, 12);
}

    int
rl_cmd_accept_version(buf)
    buf_T *buf;
{
    char data[20];
    rl_save_uint64(data     , 20);
    rl_save_uint32(data +  8, RL_CMD_ACCEPTED);
    rl_save_uint64(data + 12, buf->b_ml.ml_remote.ml_incoming_version);

    return rl_write_all(buf->b_ml.ml_remote.ml_socket, data, 20);
}

    int
rl_write_all(fd, data, len)
    int	  fd;
    char  *data;
    int   len;
{
    while(len)
    {
        int r = write(fd, data, len);
        if(r < 0)
        {
            return FAIL;
        }

        data += r;
        len -= r;
    }

    return OK;
}

    void
rl_wait(buf)
    buf_T *buf;
{
    rl_checkpoint(buf);

    while(buf->b_ml.ml_remote.ml_checkpoint_sent != buf->b_ml.ml_remote.ml_checkpoint_recv)
        rl_receive(buf);
}

    void
rl_receive(buf)
    buf_T *buf;
{
    remote_memline_T *rm = &buf->b_ml.ml_remote;

    int r = read(rm->ml_socket,
              rm->in_buf_fill,
              IN_BUF_SIZE - (rm->in_buf_fill - rm->in_buf));
    // fprintf(stderr, "Read: %d\n", r);
    if(r < 0)
    {
        EMSG2("read from remote master failed, disconnecting: %s", strerror(errno));
        rl_disconnect_remote(buf);
        return;
    }
    if(r == 0)
    {
        EMSG("EOF from remote master, disconnecting");
        rl_disconnect_remote(buf);
        return;
    }

    rm->in_buf_fill += r;

    while(1)
    {
        long recv_len = rm->in_buf_fill - rm->in_buf;
        long packet_len = rl_load_uint64(rm->in_buf);

        if(recv_len < packet_len)
            break;

        char_u *pkg = rm->in_buf;

        int cmd = rl_load_uint32(pkg + 8);
        // fprintf(stderr, "Received cmd: %d\n", cmd);

        switch(cmd)
        {
            case RL_CMD_CLEAR:
                {
                    while(rm->ml_incoming.line_count)
                        rl_delete(&rm->ml_incoming, 1);
                }
                break;
            case RL_CMD_APPEND_NOSYNC:
            case RL_CMD_APPEND:
                {
                    int lnum = rl_load_uint64(pkg + 12);
                    int len = rl_load_uint64(pkg + 20);

                    rl_append(&rm->ml_incoming, lnum, pkg + 28, len);
                }
                break;
            case RL_CMD_DELETE:
                {
                    int lnum = rl_load_uint64(pkg + 12);

                    rl_delete(&rm->ml_incoming, lnum);
                }
                break;
            case RL_CMD_MASTER_COMPLETE:
                {
                    int ver = rl_load_uint64(pkg + 12);

                    buf->b_ml.ml_remote.ml_incoming_version = ver;

                    if(rm->ml_checkpoint_recv == rm->ml_checkpoint_cur)
                        ml_update_to_remote_version(buf);
                }
                break;
            case RL_CMD_CHECKPOINT_REACH:
                {
                    int cp = rl_load_uint64(pkg + 12);

                    rm->ml_checkpoint_recv = cp;

                    if(rm->ml_checkpoint_recv == rm->ml_checkpoint_cur)
                        ml_update_to_remote_version(buf);
                }
                break;
            default:
                {
                    EMSG("Unknown / invalid message received from master");
                }
                break;
        }

        memmove(rm->in_buf, rm->in_buf + packet_len,
                rm->in_buf_fill - rm->in_buf - packet_len);
        rm->in_buf_fill -= packet_len;
    }
}

    long
rl_load_uint64(data)
    char_u *data;
{
    return ((long)data[0] << 0) +
           ((long)data[1] << 8) +
           ((long)data[2] << 16) +
           ((long)data[3] << 24) +
           ((long)data[4] << 32) +
           ((long)data[5] << 40) +
           ((long)data[6] << 48) +
           ((long)data[7] << 56);
}

    long
rl_load_uint32(data)
    char_u *data;
{
    return ((long)data[0] << 0) +
           ((long)data[1] << 8) +
           ((long)data[2] << 16) +
           ((long)data[3] << 24);
}

rl_save_uint64(data, n)
    char_u *data;
    long n;
{
    data[0] = (n >> 0) & 0xff;
    data[1] = (n >> 8) & 0xff;
    data[2] = (n >> 16) & 0xff;
    data[3] = (n >> 24) & 0xff;
    data[4] = (n >> 32) & 0xff;
    data[5] = (n >> 40) & 0xff;
    data[6] = (n >> 48) & 0xff;
    data[7] = (n >> 56) & 0xff;
}

rl_save_uint32(data, n)
    char_u *data;
    long n;
{
    data[0] = (n >> 0) & 0xff;
    data[1] = (n >> 8) & 0xff;
    data[2] = (n >> 16) & 0xff;
    data[3] = (n >> 24) & 0xff;
}

    int
rl_append(rl, lnum, line, len)
    remline_T	*rl;
    int		lnum;
    char_u	*line;
    int		len;
{
    int i;

    char_u *new_data = malloc(len + 1);
    if(!new_data)
        return FAIL;

    memcpy(new_data, line, len);
    new_data[len] = '\0';

    // TODO: handle failure
    rl->lines = realloc(rl->lines, sizeof(remline_line_T) * (rl->line_count + 1));
    memmove(rl->lines + lnum + 1, rl->lines + lnum, sizeof(remline_line_T) * (rl->line_count - lnum));

    rl->lines[lnum].len = len;
    rl->lines[lnum].data = new_data;
    rl->lines[lnum].local_lnum = -1;

    ++rl->line_count;

    return OK;
}

    int
rl_delete(rl, lnum)
    remline_T	*rl;
    int		lnum;
{
    int i;

    --lnum;

    free(rl->lines[lnum].data);

    memmove(rl->lines + lnum, rl->lines + lnum + 1, sizeof(remline_line_T) * (rl->line_count - lnum - 1));

    // TODO: handle failure
    rl->lines = realloc(rl->lines, sizeof(remline_line_T) * (rl->line_count - 1));

    --rl->line_count;

    return OK;
}

    void
rl_init(rl)
    remline_T	*rl;
{
    rl->line_count = 0;
    rl->lines = NULL;
}

    void
rl_copy(dst, src)
    remline_T *dst;
    remline_T *src;
{
    int i;

    for(i = 0; i < dst->line_count; ++i)
        free(dst->lines[i].data);
    free(dst->lines);

    // FIXME: handle errors
    dst->line_count = src->line_count;
    dst->lines = malloc(sizeof(remline_line_T) * dst->line_count);

    for(i = 0; i < src->line_count; ++i)
    {
        dst->lines[i].len = src->lines[i].len;
        dst->lines[i].data = malloc(src->lines[i].len);
        memcpy(dst->lines[i].data, src->lines[i].data, src->lines[i].len);
    }
}

    int
rl_socket(buf)
    buf_T *buf;
{
    if(!ml_has_remote(buf))
        return -1;

    return buf->b_ml.ml_remote.ml_socket;
}

    int
rl_checkpoint(buf)
    buf_T *buf;
{
    int lnum = -1;
    if(buf == curwin->w_buffer)
        lnum = curwin->w_cursor.lnum;

    if(!ml_has_remote(buf))
        return;

    if(buf->b_ml.ml_remote.ml_checkpoint_cur ==
            buf->b_ml.ml_remote.ml_checkpoint_sent) 
        return;

    buf->b_ml.ml_remote.ml_checkpoint_sent =
        buf->b_ml.ml_remote.ml_checkpoint_cur;

    char_u data[20];
    rl_save_uint64(data     , 20);
    rl_save_uint32(data +  8, RL_CMD_CHECKPOINT);
    rl_save_uint64(data + 12, buf->b_ml.ml_remote.ml_checkpoint_sent);

    return rl_write_all(buf->b_ml.ml_remote.ml_socket, data, 20);
}
