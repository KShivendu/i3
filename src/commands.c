/*
 * vim:ts=8:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 *
 * © 2009 Michael Stapelberg and contributors
 *
 * See file LICENSE for license information.
 *
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include <xcb/xcb.h>

#include "util.h"
#include "data.h"
#include "table.h"
#include "layout.h"
#include "i3.h"
#include "xinerama.h"

static bool focus_window_in_container(xcb_connection_t *conn, Container *container,
                direction_t direction) {
        /* If this container is empty, we’re done */
        if (container->currently_focused == NULL)
                return false;

        /* Get the previous/next client or wrap around */
        Client *candidate = NULL;
        if (direction == D_UP) {
                if ((candidate = CIRCLEQ_PREV_OR_NULL(&(container->clients), container->currently_focused, clients)) == NULL)
                        candidate = CIRCLEQ_LAST(&(container->clients));
        }
        else if (direction == D_DOWN) {
                if ((candidate = CIRCLEQ_NEXT_OR_NULL(&(container->clients), container->currently_focused, clients)) == NULL)
                        candidate = CIRCLEQ_FIRST(&(container->clients));
        } else printf("Direction not implemented!\n");

        /* Set focus */
        set_focus(conn, candidate);

        return true;
}

typedef enum { THING_WINDOW, THING_CONTAINER } thing_t;

static void focus_thing(xcb_connection_t *conn, direction_t direction, thing_t thing) {
        printf("focusing direction %d\n", direction);

        int new_row = current_row,
            new_col = current_col;

        Container *container = CUR_CELL;
        Workspace *t_ws = c_ws;

        /* There always is a container. If not, current_col or current_row is wrong */
        assert(container != NULL);

        /* TODO: for horizontal default layout, this has to be expanded to LEFT/RIGHT */
        if (direction == D_UP || direction == D_DOWN) {
                if (thing == THING_WINDOW)
                        /* Let’s see if we can perform up/down focus in the current container */
                        if (focus_window_in_container(conn, container, direction))
                                return;

                if (direction == D_DOWN && cell_exists(current_col, current_row+1))
                        new_row++;
                else if (direction == D_UP && cell_exists(current_col, current_row-1))
                        new_row--;
                else {
                        /* Let’s see if there is a screen down/up there to which we can switch */
                        printf("container is at %d with height %d\n", container->y, container->height);
                        i3Screen *screen;
                        int destination_y = (direction == D_UP ? (container->y - 1) : (container->y + container->height + 1));
                        if ((screen = get_screen_containing(container->x, destination_y)) == NULL) {
                                printf("Not possible, no screen found.\n");
                                return;
                        }
                        t_ws = &(workspaces[screen->current_workspace]);
                        new_row = (direction == D_UP ? (t_ws->rows - 1) : 0);
                        /* Bounds checking */
                        if (new_col >= t_ws->cols)
                                new_col = (t_ws->cols - 1);
                        if (new_row >= t_ws->rows)
                                new_row = (t_ws->rows - 1);
                }
        } else if (direction == D_LEFT || direction == D_RIGHT) {
                if (direction == D_RIGHT && cell_exists(current_col+1, current_row))
                        new_col++;
                else if (direction == D_LEFT && cell_exists(current_col-1, current_row))
                        new_col--;
                else {
                        /* Let’s see if there is a screen left/right here to which we can switch */
                        printf("container is at %d with width %d\n", container->x, container->width);
                        i3Screen *screen;
                        int destination_x = (direction == D_LEFT ? (container->x - 1) : (container->x + container->width + 1));
                        if ((screen = get_screen_containing(destination_x, container->y)) == NULL) {
                                printf("Not possible, no screen found.\n");
                                return;
                        }
                        t_ws = &(workspaces[screen->current_workspace]);
                        new_col = (direction == D_LEFT ? (t_ws->cols - 1) : 0);
                        /* Bounds checking */
                        if (new_col >= t_ws->cols)
                                new_col = (t_ws->cols - 1);
                        if (new_row >= t_ws->rows)
                                new_row = (t_ws->rows - 1);
                }
        } else {
                printf("direction unhandled\n");
                return;
        }

        if (t_ws->table[new_col][new_row]->currently_focused != NULL)
                set_focus(conn, t_ws->table[new_col][new_row]->currently_focused);
}

/*
 * Tries to move the window inside its current container.
 *
 * Returns true if the window could be moved, false otherwise.
 *
 */
static bool move_current_window_in_container(xcb_connection_t *conn, Client *client,
                direction_t direction) {
        assert(client->container != NULL);

        Client *other = (direction == D_UP ? CIRCLEQ_PREV(client, clients) :
                                             CIRCLEQ_NEXT(client, clients));

        if (other == CIRCLEQ_END(&(client->container->clients)))
                return false;

        printf("i can do that\n");
        /* We can move the client inside its current container */
        CIRCLEQ_REMOVE(&(client->container->clients), client, clients);
        if (direction == D_UP)
                CIRCLEQ_INSERT_BEFORE(&(client->container->clients), other, client, clients);
        else CIRCLEQ_INSERT_AFTER(&(client->container->clients), other, client, clients);
        render_layout(conn);
        return true;
}

/*
 * Moves the current window to the given direction, creating a column/row if
 * necessary
 *
 */
static void move_current_window(xcb_connection_t *conn, direction_t direction) {
        printf("moving window to direction %s\n", (direction == D_UP ? "up" : (direction == D_DOWN ? "down" :
                                        (direction == D_LEFT ? "left" : "right"))));
        /* Get current window */
        Container *container = CUR_CELL,
                  *new = NULL;

        /* There has to be a container, see focus_window() */
        assert(container != NULL);

        /* If there is no window or the dock window is focused, we’re done */
        if (container->currently_focused == NULL ||
            container->currently_focused->dock)
                return;

        /* As soon as the client is moved away, the next client in the old
         * container needs to get focus, if any. Therefore, we save it here. */
        Client *current_client = container->currently_focused;
        Client *to_focus = CIRCLEQ_NEXT_OR_NULL(&(container->clients), current_client, clients);
        if (to_focus == NULL)
                to_focus = CIRCLEQ_PREV_OR_NULL(&(container->clients), current_client, clients);

        switch (direction) {
                case D_LEFT:
                        /* TODO: If we’re at the left-most position, move the rest of the table right */
                        if (current_col == 0)
                                return;

                        new = CUR_TABLE[--current_col][current_row];
                        break;
                case D_RIGHT:
                        if (current_col == (c_ws->cols-1))
                                expand_table_cols(c_ws);

                        new = CUR_TABLE[++current_col][current_row];
                        break;
                case D_UP:
                        /* TODO: if we’re at the up-most position, move the rest of the table down */
                        if (move_current_window_in_container(conn, current_client, D_UP) ||
                                current_row == 0)
                                return;

                        new = CUR_TABLE[current_col][--current_row];
                        break;
                case D_DOWN:
                        if (move_current_window_in_container(conn, current_client, D_DOWN))
                                return;

                        if (current_row == (c_ws->rows-1))
                                expand_table_rows(c_ws);

                        new = CUR_TABLE[current_col][++current_row];
                        break;
        }

        /* Remove it from the old container and put it into the new one */
        CIRCLEQ_REMOVE(&(container->clients), current_client, clients);
        CIRCLEQ_INSERT_TAIL(&(new->clients), current_client, clients);

        /* Update data structures */
        current_client->container = new;
        container->currently_focused = to_focus;
        new->currently_focused = current_client;

        /* delete all empty columns/rows */
        cleanup_table(conn, container->workspace);
        render_layout(conn);

        set_focus(conn, current_client);
}

/*
 * "Snaps" the current container (not possible for windows, because it works at table base)
 * to the given direction, that is, adjusts cellspan/rowspan
 *
 */
static void snap_current_container(xcb_connection_t *conn, direction_t direction) {
        printf("snapping container to direction %d\n", direction);

        Container *container = CUR_CELL;

        assert(container != NULL);

        switch (direction) {
                case D_LEFT:
                        /* Snap to the left is actually a move to the left and then a snap right */
                        move_current_window(conn, D_LEFT);
                        snap_current_container(conn, D_RIGHT);
                        return;
                case D_RIGHT:
                        /* Check if the cell is used */
                        if (!cell_exists(container->col + 1, container->row) ||
                                CUR_TABLE[container->col+1][container->row]->currently_focused != NULL) {
                                printf("cannot snap to right - the cell is already used\n");
                                return;
                        }

                        /* Check if there are other cells with rowspan, which are in our way.
                         * If so, reduce their rowspan. */
                        for (int i = container->row-1; i >= 0; i--) {
                                printf("we got cell %d, %d with rowspan %d\n",
                                                container->col+1, i, CUR_TABLE[container->col+1][i]->rowspan);
                                while ((CUR_TABLE[container->col+1][i]->rowspan-1) >= (container->row - i))
                                        CUR_TABLE[container->col+1][i]->rowspan--;
                                printf("new rowspan = %d\n", CUR_TABLE[container->col+1][i]->rowspan);
                        }

                        container->colspan++;
                        break;
                case D_UP:
                        move_current_window(conn, D_UP);
                        snap_current_container(conn, D_DOWN);
                        return;
                case D_DOWN:
                        printf("snapping down\n");
                        if (!cell_exists(container->col, container->row+1) ||
                                CUR_TABLE[container->col][container->row+1]->currently_focused != NULL) {
                                printf("cannot snap down - the cell is already used\n");
                                return;
                        }

                        for (int i = container->col-1; i >= 0; i--) {
                                printf("we got cell %d, %d with colspan %d\n",
                                                i, container->row+1, CUR_TABLE[i][container->row+1]->colspan);
                                while ((CUR_TABLE[i][container->row+1]->colspan-1) >= (container->col - i))
                                        CUR_TABLE[i][container->row+1]->colspan--;
                                printf("new colspan = %d\n", CUR_TABLE[i][container->row+1]->colspan);

                        }

                        container->rowspan++;
                        break;
        }

        render_layout(conn);
}

/*
 * Moves the currently selected window to the given workspace
 *
 */
static void move_current_window_to_workspace(xcb_connection_t *conn, int workspace) {
        printf("Moving current window to workspace %d\n", workspace);

        Container *container = CUR_CELL;

        assert(container != NULL);

        /* t_ws (to workspace) is just a container pointer to the workspace we’re switching to */
        Workspace *t_ws = &(workspaces[workspace-1]);

        Client *current_client = container->currently_focused;
        if (current_client == NULL) {
                printf("No currently focused client in current container.\n");
                return;
        }
        Client *to_focus = CIRCLEQ_NEXT_OR_NULL(&(container->clients), current_client, clients);
        if (to_focus == NULL)
                to_focus = CIRCLEQ_PREV_OR_NULL(&(container->clients), current_client, clients);

        if (t_ws->screen == NULL) {
                printf("initializing new workspace, setting num to %d\n", workspace-1);
                t_ws->screen = container->workspace->screen;
                /* Copy the dimensions from the virtual screen */
		memcpy(&(t_ws->rect), &(container->workspace->screen->rect), sizeof(Rect));
        }

        Container *to_container = t_ws->table[t_ws->current_col][t_ws->current_row];

        assert(to_container != NULL);

        CIRCLEQ_REMOVE(&(container->clients), current_client, clients);
        SLIST_REMOVE(&(container->workspace->focus_stack), current_client, Client, focus_clients);

        CIRCLEQ_INSERT_TAIL(&(to_container->clients), current_client, clients);
        SLIST_INSERT_HEAD(&(to_container->workspace->focus_stack), current_client, focus_clients);
        printf("Moved.\n");

        current_client->container = to_container;
        container->currently_focused = to_focus;
        to_container->currently_focused = current_client;

        /* If we’re moving it to an invisible screen, we need to unmap it */
        if (to_container->workspace->screen->current_workspace != to_container->workspace->num) {
                printf("This workspace is not visible, unmapping\n");
                xcb_unmap_window(conn, current_client->frame);
        }

        /* delete all empty columns/rows */
        cleanup_table(conn, container->workspace);

        render_layout(conn);
}

static void show_workspace(xcb_connection_t *conn, int workspace) {
        Client *client;
        xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root;
        /* t_ws (to workspace) is just a convenience pointer to the workspace we’re switching to */
        Workspace *t_ws = &(workspaces[workspace-1]);

        printf("show_workspace(%d)\n", workspace);

        /* Store current_row/current_col */
        c_ws->current_row = current_row;
        c_ws->current_col = current_col;

        /* Check if the workspace has not been used yet */
        if (t_ws->screen == NULL) {
                printf("initializing new workspace, setting num to %d\n", workspace);
                t_ws->screen = c_ws->screen;
                /* Copy the dimensions from the virtual screen */
		memcpy(&(t_ws->rect), &(t_ws->screen->rect), sizeof(Rect));
        }

        if (c_ws->screen != t_ws->screen) {
                /* We need to switch to the other screen first */
                printf("moving over to other screen.\n");
                c_ws = &(workspaces[t_ws->screen->current_workspace]);
                current_col = c_ws->current_col;
                current_row = c_ws->current_row;
                if (CUR_CELL->currently_focused != NULL)
                        warp_pointer_into(conn, CUR_CELL->currently_focused);
                else {
                        Rect *dims = &(c_ws->screen->rect);
                        xcb_warp_pointer(conn, XCB_NONE, root, 0, 0, 0, 0,
                                         dims->x + (dims->width / 2), dims->y + (dims->height / 2));
                }
        }

        /* Check if we need to change something or if we’re already there */
        if (c_ws->screen->current_workspace == (workspace-1))
                return;

        t_ws->screen->current_workspace = workspace-1;

        /* TODO: does grabbing the server actually bring us any (speed)advantages? */
        //xcb_grab_server(conn);

        /* Unmap all clients of the current workspace */
        int unmapped_clients = 0;
        for (int cols = 0; cols < c_ws->cols; cols++)
                for (int rows = 0; rows < c_ws->rows; rows++)
                        CIRCLEQ_FOREACH(client, &(c_ws->table[cols][rows]->clients), clients) {
                                xcb_unmap_window(conn, client->frame);
                                unmapped_clients++;
                        }

        /* If we did not unmap any clients, the workspace is empty and we can destroy it */
        if (unmapped_clients == 0)
                c_ws->screen = NULL;

        /* Unmap the stack windows on the current workspace, if any */
        struct Stack_Window *stack_win;
        SLIST_FOREACH(stack_win, &stack_wins, stack_windows)
                if (stack_win->container->workspace == c_ws)
                        xcb_unmap_window(conn, stack_win->window);

        c_ws = &workspaces[workspace-1];
        current_row = c_ws->current_row;
        current_col = c_ws->current_col;
        printf("new current row = %d, current col = %d\n", current_row, current_col);

        /* Map all clients on the new workspace */
        for (int cols = 0; cols < c_ws->cols; cols++)
                for (int rows = 0; rows < c_ws->rows; rows++)
                        CIRCLEQ_FOREACH(client, &(c_ws->table[cols][rows]->clients), clients)
                                xcb_map_window(conn, client->frame);

        /* Map all stack windows, if any */
        SLIST_FOREACH(stack_win, &stack_wins, stack_windows)
                if (stack_win->container->workspace == c_ws)
                        xcb_map_window(conn, stack_win->window);

        /* Restore focus on the new workspace */
        if (CUR_CELL->currently_focused != NULL)
                set_focus(conn, CUR_CELL->currently_focused);
        else xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, root, XCB_CURRENT_TIME);

        //xcb_ungrab_server(conn);

        render_layout(conn);
}

/*
 * Parses a command, see file CMDMODE for more information
 *
 */
void parse_command(xcb_connection_t *conn, const char *command) {
        printf("--- parsing command \"%s\" ---\n", command);
        /* Hmm, just to be sure */
        if (command[0] == '\0')
                return;

        /* Is it an <exec>? */
        if (strncmp(command, "exec ", strlen("exec ")) == 0) {
                printf("starting \"%s\"\n", command + strlen("exec "));
                start_application(command+strlen("exec "));
                return;
        }

        /* Is it <restart>? */
        if (strncmp(command, "restart", strlen("restart")) == 0) {
                printf("restarting \"%s\"...\n", application_path);
                execl(application_path, application_path, NULL);
                /* not reached */
        }

        /* Is it 'f' for fullscreen? */
        if (command[0] == 'f') {
                if (CUR_CELL->currently_focused == NULL)
                        return;
                toggle_fullscreen(conn, CUR_CELL->currently_focused);
                return;
        }

        /* Is it just 's' for stacking or 'd' for default? */
        if ((command[0] == 's' || command[0] == 'd') && (command[1] == '\0')) {
                printf("Switching mode for current container\n");
                switch_layout_mode(conn, CUR_CELL, (command[0] == 's' ? MODE_STACK : MODE_DEFAULT));
                return;
        }

        enum { WITH_WINDOW, WITH_CONTAINER } with = WITH_WINDOW;

        /* Is it a <with>? */
        if (command[0] == 'w') {
                command++;
                /* TODO: implement */
                if (command[0] == 'c') {
                        with = WITH_CONTAINER;
                        command++;
                } else {
                        printf("not yet implemented.\n");
                        return;
                }
        }

        /* It's a normal <cmd> */
        char *rest = NULL;
        enum { ACTION_FOCUS, ACTION_MOVE, ACTION_SNAP } action = ACTION_FOCUS;
        direction_t direction;
        int times = strtol(command, &rest, 10);
        if (rest == NULL) {
                printf("Invalid command (\"%s\")\n", command);
                return;
        }

        if (*rest == '\0') {
                /* No rest? This was a tag number, not a times specification */
                show_workspace(conn, times);
                return;
        }

        if (*rest == 'm' || *rest == 's') {
                action = (*rest == 'm' ? ACTION_MOVE : ACTION_SNAP);
                rest++;
        }

        int workspace = strtol(rest, &rest, 10);

        if (rest == NULL) {
                printf("Invalid command (\"%s\")\n", command);
                return;
        }

        if (*rest == '\0') {
                move_current_window_to_workspace(conn, workspace);
                return;
        }

        /* Now perform action to <where> */
        while (*rest != '\0') {
                if (*rest == 'h')
                        direction = D_LEFT;
                else if (*rest == 'j')
                        direction = D_DOWN;
                else if (*rest == 'k')
                        direction = D_UP;
                else if (*rest == 'l')
                        direction = D_RIGHT;
                else {
                        printf("unknown direction: %c\n", *rest);
                        return;
                }

                if (action == ACTION_FOCUS) {
                        focus_thing(conn, direction, (with == WITH_WINDOW ? THING_WINDOW : THING_CONTAINER));
                }
                else if (action == ACTION_MOVE)
                        move_current_window(conn, direction);
                else if (action == ACTION_SNAP)
                        snap_current_container(conn, direction);

                rest++;
        }

        printf("--- done ---\n");
}
