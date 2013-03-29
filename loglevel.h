/* loglevel.h - routines to modify kernel console loglevel
 *
 * Released under GPL v2.
 * (c) 2007 Tim Dijkstra
 */

void open_printk(void);
int get_kernel_console_loglevel(void);
void set_kernel_console_loglevel(int level);
void close_printk(void);
