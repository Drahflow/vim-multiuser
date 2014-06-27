/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 */

// FIXME: also implement an EtherpadLite connector.
// FIXME: authentication

#include "vim.h"
#include "multiuser.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#define IN_BUF_SIZE 1000000

typedef struct line_map
{
    int line_count;	/* total number of lines */
    int *map;		/* target line numbers, -1 if line is not mapped, 1-based as everywhere */
} line_map_T;

typedef struct version_map
{
    int		start_version;		/* the first version a map exists for */
    int		end_version;		/* the last version a map exists for + 1 */
    line_map_T	*maps;			/* the actual line_map_T array */
} version_map_T;

typedef struct multiuser_connection
{
    int		mc_socket;			/* the file descriptor of the network connection */
    char_u	mc_in_buf[IN_BUF_SIZE];		/* network input buffer */
    char_u	*mc_in_buf_fill;		/* the next byte to read from the network */
    line_map_T	mc_line_map;			/* mapping from client to master lines */
    int		mc_version;			/* the last version the client accepted */
} multiuser_connection_T;

typedef multiuser_connection_T con_T;

static con_T *cons = NULL;
static int con_count = 0;

static remline_T master;
static version_map_T versions;

/*
 * return value
 *  0 .. n: a valid master line number
 * -1     : no master line existed
 * -2     : an error occurred
 */
    static int
multiuser_master_lnum(lmap, lnum)
    line_map_T *lmap;
    int        lnum;
{
    if(lnum == 0)
        return 0;

    if(lmap->map)
    {
        if(lnum > lmap->line_count)
            return -2;

        return lmap->map[lnum];
    }

    return -2;
}

    static int
multiuser_realloc_cons(n)
    int n;
{
    int i;
    int max = con_count < n? con_count: n;

    con_T *new_cons = malloc(sizeof(con_T) * n);
    if(!new_cons)
        return FAIL;

    memcpy(new_cons, cons, sizeof(con_T) * con_count);
    for(i = 0; i < max; ++i)
    {
        new_cons[i].mc_in_buf_fill =
            new_cons[i].mc_in_buf +
            (cons[i].mc_in_buf_fill - cons[i].mc_in_buf);
    }

    free(cons);
    cons = new_cons;
    con_count = n;

    return OK;
}

    void
multiuser_server()
{
    int i;

    rl_init(&master);
    rl_append(&master, 0, "", 0);

    versions.start_version = 1;
    versions.end_version = 2;
    versions.maps = malloc(sizeof(line_map_T));
    if(!versions.maps)
    {
        fprintf(stderr, "Cannot allocate version map buffers.\n");
        exit(1);
    }
    versions.maps[0].line_count = 1;
    versions.maps[0].map = malloc(sizeof(int) * 2);
    if(!versions.maps[0].map)
    {
        fprintf(stderr, "Cannot allocate version map buffers.\n");
        exit(1);
    }
    versions.maps[0].map[0] = 0;
    versions.maps[0].map[1] = 1;

    int main_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(main_socket < 0)
    {
        fprintf(stderr, "Failed to socket(2): %s\n", strerror(errno));
        exit(1);
    }

    int one = 1;
    int sso = setsockopt(main_socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(sso < 0)
    {
        fprintf(stderr, "Failed to setsockopt(2): %s\n", strerror(errno));
        exit(1);
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9999);
    addr.sin_addr.s_addr = INADDR_ANY;

    int b = bind(main_socket, (struct sockaddr *)&addr, sizeof(addr));
    if(b < 0)
    {
        fprintf(stderr, "Failed to bind(2): %s\n", strerror(errno));
        exit(1);
    }

    int l = listen(main_socket, 32);
    if(l < 0)
    {
        fprintf(stderr, "Failed to listen(2): %s\n", strerror(errno));
        exit(1);
    }

    while(1)
    {
        fd_set readfds;
        int nfds = 0;
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        FD_ZERO(&readfds);
        FD_SET(main_socket, &readfds);
        nfds = main_socket + 1;

        for(i = 0; i < con_count; ++i)
        {
            FD_SET(cons[i].mc_socket, &readfds);
            if(cons[i].mc_socket + 1 > nfds)
                nfds = cons[i].mc_socket + 1;
        }

        int r = select(nfds, &readfds, NULL, NULL, &timeout);
        if(r < 0)
        {
            fprintf(stderr, "Failed to select(2): %s\n", strerror(errno));
            exit(1);
        }

        printf("Tick.\n");

        if(FD_ISSET(main_socket, &readfds))
        {
            int con = accept(main_socket, NULL, NULL);

            printf("Connection established.\n");

            multiuser_realloc_cons(con_count + 1); // TODO: handle failure

            cons[con_count - 1].mc_socket = con;
            cons[con_count - 1].mc_in_buf_fill = cons[con_count - 1].mc_in_buf;
            cons[con_count - 1].mc_line_map.line_count = 0;
            cons[con_count - 1].mc_line_map.map = malloc(sizeof(int) * 1);
            cons[con_count - 1].mc_line_map.map[0] = 0;
            cons[con_count - 1].mc_version = -1;
        }

        for(i = 0; i < con_count; ++i)
        {
            if(!FD_ISSET(cons[i].mc_socket, &readfds)) continue;

            int l = read(cons[i].mc_socket, cons[i].mc_in_buf_fill,
                    IN_BUF_SIZE - (cons[i].mc_in_buf_fill - cons[i].mc_in_buf));
            if(l < 0)
            {
                printf("Connection error: %s\n", strerror(errno));
                free(cons[i].mc_line_map.map);

                memcpy(&cons[i], &cons[con_count - 1], sizeof(con_T));
                cons[i].mc_in_buf_fill =
                    cons[con_count - 1].mc_in_buf_fill - cons[con_count - 1].mc_in_buf
                    + cons[i].mc_in_buf;

                --con_count;
                // memory will be freed upon next new connection
                break;
            }
            else if(l == 0)
            {
                printf("Connection lost.\n");
                free(cons[i].mc_line_map.map);

                memcpy(&cons[i], &cons[con_count - 1], sizeof(con_T));
                cons[i].mc_in_buf_fill =
                    cons[con_count - 1].mc_in_buf_fill - cons[con_count - 1].mc_in_buf
                    + cons[i].mc_in_buf;

                --con_count;
                // memory will be freed upon next new connection
                break;
            }
            else
            {
                cons[i].mc_in_buf_fill += l;

                multiuser_after_read(i);
            }
        }
    }
}

    void
multiuser_after_read(con)
    int con;
{
    int i;
    int j;

    while(1)
    {
        long recv_len = cons[con].mc_in_buf_fill - cons[con].mc_in_buf;
        long packet_len = rl_load_uint64(cons[con].mc_in_buf);

        char_u *pkg = cons[con].mc_in_buf;

        if(recv_len < packet_len)
            break;

        int cmd = rl_load_uint32(pkg + 8);
        printf("Received command %d.\n", cmd);

        switch(cmd)
        {
            case RL_CMD_REQ_FULL_TRANS:
                {
                    char_u data[20];

                    rl_save_uint64(data, 12);
                    rl_save_uint32(data + 8, RL_CMD_CLEAR);
                    rl_write_all(cons[con].mc_socket, data, 12);

                    for(i = 0; i < master.line_count; ++i)
                    {
                        rl_save_uint64(data, 28 + master.lines[i].len);
                        rl_save_uint32(data + 8, RL_CMD_APPEND);
                        rl_save_uint64(data + 12, i);
                        rl_save_uint64(data + 20, master.lines[i].len);

                        rl_write_all(cons[con].mc_socket, data, 28);
                        rl_write_all(cons[con].mc_socket, master.lines[i].data, master.lines[i].len);
                    }

                    rl_save_uint64(data, 20);
                    rl_save_uint32(data + 8, RL_CMD_MASTER_COMPLETE);
                    rl_save_uint64(data + 12, versions.end_version - 1);
                    rl_write_all(cons[con].mc_socket, data, 20);
                }
                break;
            case RL_CMD_APPEND_NOSYNC:
            case RL_CMD_APPEND:
                {
                    int lnum = rl_load_uint64(pkg + 12);
                    int len = rl_load_uint64(pkg + 20);
                    printf("... append after line %d\n", lnum);

                    int master_lnum = multiuser_master_lnum(&cons[con].mc_line_map, lnum);
                    printf("... aka master line %d\n", master_lnum);

                    if(master_lnum == -2)
                    {
                        fprintf(stderr, "Invalid line number received\n");
                        break;
                    }

                    if(master_lnum < 0)
                        break;

                    rl_append(&master, master_lnum, pkg + 28, len);

                    // adjust client line maps
                    for(i = 0; i < con_count; ++i)
                    {
                        line_map_T *map = &cons[i].mc_line_map;
                        for(j = 1; j < map->line_count + 1; ++j)
                            if(map->map[j] > master_lnum)
                                ++map->map[j];
                    }
                    
                    // map the new line in current view of client
                    ++cons[con].mc_line_map.line_count;
                    cons[con].mc_line_map.map = realloc(cons[con].mc_line_map.map,
                            sizeof(int) * (cons[con].mc_line_map.line_count + 1));
                    memmove(cons[con].mc_line_map.map + lnum + 2,
                            cons[con].mc_line_map.map + lnum + 1,
                            sizeof(int) * (cons[con].mc_line_map.line_count - lnum - 1));
                    cons[con].mc_line_map.map[lnum + 1] = master_lnum + 1;

                    // printf("= line map =\n");
                    // for(i = 0; i < cons[con].mc_line_map.line_count + 1; ++i)
                    // {
                    //     printf("[%d] = %d\n", i, cons[con].mc_line_map.map[i]);
                    // }

                    // adjust version line maps
                    for(i = 0; i < versions.end_version - versions.start_version; ++i)
                    {
                        line_map_T *map = &versions.maps[i];
                        for(j = 1; j < map->line_count + 1; ++j)
                            if(map->map[j] > master_lnum)
                                ++map->map[j];
                    }

                    // forward update to all clients
                    rl_save_uint64(pkg + 12, master_lnum);
                    for(i = 0; i < con_count; ++i)
                        rl_write_all(cons[i].mc_socket, pkg, packet_len);

                    multiuser_release_version(cmd != RL_CMD_APPEND_NOSYNC);
                }
                break;
            case RL_CMD_DELETE:
                {
                    int lnum = rl_load_uint64(pkg + 12);
                    printf("... delete line %d\n", lnum);

                    int master_lnum = multiuser_master_lnum(&cons[con].mc_line_map, lnum);
                    printf("... aka master line %d\n", master_lnum);

                    if(master_lnum == -2)
                    {
                        fprintf(stderr, "Invalid line number received\n");
                        break;
                    }

                    if(master_lnum == 0)
                    {
                        fprintf(stderr, "Invalid (zero) line number received\n");
                        break;
                    }

                    if(master_lnum < 0)
                        break;

                    rl_delete(&master, master_lnum);

                    // adjust client line maps
                    for(i = 0; i < con_count; ++i)
                    {
                        line_map_T *map = &cons[i].mc_line_map;
                        for(j = 1; j < map->line_count + 1; ++j)
                        {
                            if(map->map[j] == master_lnum)
                                map->map[j] = -1;
                            if(map->map[j] > master_lnum)
                                --map->map[j];
                        }
                    }

                    // remove the line from the sending clients view
                    --cons[con].mc_line_map.line_count;
                    memmove(cons[con].mc_line_map.map + lnum,
                            cons[con].mc_line_map.map + lnum + 1,
                            sizeof(int) * (cons[con].mc_line_map.line_count - lnum + 1));
                    cons[con].mc_line_map.map = realloc(cons[con].mc_line_map.map,
                            sizeof(int) * (cons[con].mc_line_map.line_count + 1));

                    // printf("= line map =\n");
                    // for(i = 0; i < cons[con].mc_line_map.line_count + 1; ++i)
                    // {
                    //     printf("[%d] = %d\n", i, cons[con].mc_line_map.map[i]);
                    // }

                    // adjust version line maps
                    for(i = 0; i < versions.end_version - versions.start_version; ++i)
                    {
                        line_map_T *map = &versions.maps[i];
                        for(j = 1; j < map->line_count + 1; ++j)
                        {
                            if(map->map[j] == master_lnum)
                                map->map[j] = -1;
                            if(map->map[j] > master_lnum)
                                --map->map[j];
                        }
                    }

                    // forward update to all clients
                    rl_save_uint64(pkg + 12, master_lnum);
                    for(i = 0; i < con_count; ++i)
                        rl_write_all(cons[i].mc_socket, pkg, packet_len);

                    multiuser_release_version(TRUE);
                }
                break;
            case RL_CMD_CHECKPOINT:
                {
                    int lnum = rl_load_uint64(pkg + 20);
                    printf("... checkpoint (cursor at line %d)\n", lnum);

                    if(lnum != -1)
                    {
                        lnum = multiuser_master_lnum(&cons[con].mc_line_map, lnum);
                        printf("... aka master line %d\n", lnum);

                        if(lnum == -2)
                        {
                            fprintf(stderr, "Invalid line number received (in checkpoint)\n");
                            lnum = -1;
                        }

                        if(lnum == 0)
                        {
                            fprintf(stderr, "Invalid (zero) line number received (in checkpoint)\n");
                            lnum = -1;
                        }

                        if(lnum < 0)
                        {
                            fprintf(stderr, "Invalid (negative) line number received (in checkpoint)\n");
                            lnum = -1;
                        }
                    }

                    rl_save_uint32(pkg + 8, RL_CMD_CHECKPOINT_REACH);
                    rl_save_uint64(pkg + 20, lnum);

                    rl_write_all(cons[con].mc_socket, pkg, packet_len);
                }
                break;
            case RL_CMD_ACCEPTED:
                {
                    int ver = rl_load_uint64(pkg + 12);
                    printf("... accepted version %d\n", ver);

                    if(ver < versions.start_version || ver >= versions.end_version)
                    {
                        fprintf(stderr, "Accepted version outside master range\n");
                        break;
                    }

                    line_map_T *map = &versions.maps[ver - versions.start_version];

                    // printf("= version %d line map =\n", ver);
                    // for(i = 0; i < map->line_count + 1; ++i)
                    // {
                    //     printf("[%d] = %d\n", i, map->map[i]);
                    // }

                    // copy version line map
                    free(cons[con].mc_line_map.map);

                    cons[con].mc_line_map.line_count = map->line_count;
                    cons[con].mc_line_map.map = malloc(sizeof(int) * (map->line_count + 1)); // TODO: handle failure
                    memcpy(cons[con].mc_line_map.map, map->map, sizeof(int) * (map->line_count + 1));
                    
                    // printf("= line map =\n");
                    // for(i = 0; i < cons[con].mc_line_map.line_count + 1; ++i)
                    // {
                    //     printf("[%d] = %d\n", i, cons[con].mc_line_map.map[i]);
                    // }

                    cons[con].mc_version = ver;

                    printf("version range: %d - %d\n", versions.start_version, versions.end_version - 1);

                    int min_version = versions.end_version;
                    for(i = 0; i < con_count; ++i)
                        if(cons[i].mc_version < min_version)
                            min_version = cons[i].mc_version;

                    int delta = min_version - versions.start_version;
                    // TODO: handle failure
                    line_map_T *new_maps = malloc(sizeof(line_map_T) * (versions.end_version - min_version));
                    memcpy(new_maps, versions.maps + delta, sizeof(line_map_T) * (versions.end_version - min_version));
                    
                    for(i = 0; i < delta; ++i)
                        free(versions.maps[i].map);
                    free(versions.maps);

                    versions.maps = new_maps;
                    versions.start_version = min_version;

                    printf("adjusted version range: %d - %d\n", versions.start_version, versions.end_version - 1);
                }
                break;
            default:
                {
                    fprintf(stderr, "Unknown / invalid command received\n");
                }
                break;
        }

        memmove(cons[con].mc_in_buf, cons[con].mc_in_buf + packet_len,
                cons[con].mc_in_buf_fill - cons[con].mc_in_buf - packet_len);
        cons[con].mc_in_buf_fill -= packet_len;
    }
}

    void
multiuser_release_version(send)
    int send;
{
    int i;

    ++versions.end_version;

    // TODO: handle failure
    versions.maps = realloc(versions.maps, sizeof(line_map_T) * (versions.end_version - versions.start_version));

    {
        line_map_T *curmap = &versions.maps[versions.end_version - versions.start_version - 1];
        curmap->line_count = master.line_count;

        // TODO: handle failure
        curmap->map = malloc(sizeof(int) * (curmap->line_count + 1));
        for(i = 1; i < curmap->line_count + 1; ++i)
            curmap->map[i] = i;
        curmap->map[0] = 0; // not a real master line, but stuff can be appended after 0

        // printf("= version %d line map =\n", versions.end_version - 1);
        // for(i = 0; i < curmap->line_count; ++i)
        //     printf("[%d] = %d\n", i, curmap->map[i]);
    }

    if(send) {
        char_u data[20];
        rl_save_uint64(data, 20);
        rl_save_uint32(data + 8, RL_CMD_MASTER_COMPLETE);
        rl_save_uint64(data + 12, versions.end_version - 1);

        for(i = 0; i < con_count; ++i)
            rl_write_all(cons[i].mc_socket, data, 20);
    }

    // printf("=== Version: %d ===\n", versions.end_version - 1);
    // for(i = 0; i < master.line_count; ++i)
    // {
    //     fwrite(master.lines[i].data, 1, master.lines[i].len, stdout);
    //     printf("\n");
    // }
}
