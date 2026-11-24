/* Copyright (C) 2026 Ahmed Gheith and contributors.
*
 * Use restricted to classroom projects.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <cstdint>


namespace impl
{
  // docs stolen from man poll
/**An exceptional condition has occurred on the device or socket.  This flag is output only, and ignored if present in the input events
                    bitmask. */
constexpr uint32_t POLLERR = 0x008;
/**The device or socket has been disconnected.  This flag is output only, and ignored if present in the input events bitmask.  Note that
                    POLLHUP and POLLOUT are mutually exclusive and should never be present in the revents bitmask at the same time. */
constexpr uint32_t POLLHUP = 0x010;
/**Data other than high priority data may be read without blocking.  This is equivalent to ( POLLRDNORM | POLLRDBAND ). */
constexpr uint32_t POLLIN = 0x001;
/**The file descriptor is not open.  This flag is output only, and ignored if present in the input events bitmask. */
constexpr uint32_t POLLNVAL = 0x020;
/** Normal data may be written without blocking.  This is equivalent to POLLWRNORM. */
constexpr uint32_t POLLOUT = 0x004;
/**High priority data may be read without blocking. */
constexpr uint32_t POLLPRI = 0x002;

// wake after poll_mask changes for a file so that the poller threads check again
void wake_pollers();
} // namespace impl
