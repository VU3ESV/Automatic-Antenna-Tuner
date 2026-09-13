"""PlatformIO pre-script: make FNET's DHCP client send the host name.

NativeEthernet's FNET stack (vjmuzik/FNET 0.1.3) builds DHCPDISCOVER and
DHCPREQUEST with message type, lease, parameter list, maximum size and
client id only - no Host Name option (RFC 2132 section 3.14, code 12). The
router therefore lists the controller without a name and serves no reverse
DNS entry for it, which is what IP scanners display.

This script swaps FNET's service/dhcp/fnet_dhcp_cln.c for a copy generated
into the build directory with one addition: DISCOVER and REQUEST carry
option 12 from the global `fnet_dhcp_cln_hostname`, which net_hal::begin()
sets before Ethernet.begin(). RFC 2131 table 5 forbids the option in
DECLINE and RELEASE, so those messages are unchanged.

The original file is pinned by SHA-256. A different FNET fails the build
with a message instead of silently producing a controller without a name;
re-check the anchors below against the new file and update the hash.
Builds that do not compile FNET (QNEthernet, native tests) are untouched:
the middleware only matches FNET's file.

FNET is Apache-2.0; the generated file keeps its licence header and gains
a notice describing the change.
"""
import hashlib
import os
import re
import sys

Import("env")  # noqa: F821 - injected by PlatformIO

PATTERN = "*/FNET/src/service/dhcp/fnet_dhcp_cln.c"
PINNED_SHA256 = "600a19054b6920a1ff0bc4f73f9e604aa111496ec530995bf446d501200c2495"  # vjmuzik/FNET 0.1.3

INCLUDE = '#include "fnet_dhcp_prv.h"\n'

# End of the Parameter Request List option, inside the
# `if((message_type == REQUEST) || (message_type == DISCOVER))` block of
# _fnet_dhcp_cln_send_message().
PARAM_LIST_END = re.compile(
    r"        option_position = _fnet_dhcp_add_option\([^\n]*FNET_DHCP_OPTION_PARAMETER_REQ_LIST[^\n]*\n"
    r"        if\(option_position == FNET_NULL\)\n"
    r"        \{\n"
    r"            goto EXIT;\n"
    r"        \}\n"
)

NOTICE = """
/* Automatic-Antenna-Tuner modification, generated at build time by
 * firmware/tuner-controller/tools/fnet_dhcp_hostname.py from FNET 0.1.3
 * (Apache-2.0): DHCPDISCOVER and DHCPREQUEST carry the Host Name option
 * (code 12) from fnet_dhcp_cln_hostname. No other change. */
const char *fnet_dhcp_cln_hostname = FNET_NULL;   /* set by net_hal::begin() */
"""

HOSTNAME_OPTION = """
        /* Automatic-Antenna-Tuner: Host Name option (RFC 2132 3.14) so the
         * DHCP server can register the name (router list, reverse DNS). */
        if((fnet_dhcp_cln_hostname != FNET_NULL) && (fnet_dhcp_cln_hostname[0] != '\\0'))
        {
            fnet_size_t hostname_length = fnet_strlen(fnet_dhcp_cln_hostname);
            if(hostname_length > 63U)
            {
                hostname_length = 63U;   /* one DNS label */
            }
            option_position = _fnet_dhcp_add_option(option_position, ((message->header.options + sizeof(message->header.options)) - option_position), 12U, (fnet_uint8_t)hostname_length, fnet_dhcp_cln_hostname);
            if(option_position == FNET_NULL)
            {
                goto EXIT;
            }
        }
"""


def fail(msg):
    sys.stderr.write(f"fnet_dhcp_hostname: {msg}\n")
    env.Exit(1)


def generate(original_path):
    with open(original_path, "rb") as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != PINNED_SHA256:
        fail(f"{original_path} is not the pinned FNET 0.1.3 DHCP client (sha256 {digest}). "
             "Review the patch anchors in tools/fnet_dhcp_hostname.py against the new file, "
             "then update PINNED_SHA256.")
    text = data.decode("utf-8")

    if text.count(INCLUDE) != 1:
        fail("anchor '#include \"fnet_dhcp_prv.h\"' not found exactly once")
    # The copy is compiled from the build directory, so point the quoted
    # include back at FNET's directory.
    prv = os.path.join(os.path.dirname(original_path), "fnet_dhcp_prv.h").replace(os.sep, "/")
    text = text.replace(INCLUDE, f'#include "{prv}"\n' + NOTICE)

    matches = list(PARAM_LIST_END.finditer(text))
    if len(matches) != 1:
        fail(f"Parameter Request List anchor matched {len(matches)} times, expected 1")
    end = matches[0].end()
    return text[:end] + HOSTNAME_OPTION + text[end:]


def swap_dhcp_client(lib_env, node):
    patched = generate(node.srcnode().get_abspath())
    out = os.path.join(env.subst("$BUILD_DIR"), "fnet_dhcp_hostname", "fnet_dhcp_cln.c")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    current = None
    if os.path.exists(out):
        with open(out, encoding="utf-8") as f:
            current = f.read()
    if current != patched:  # rewrite only on change so the object is not rebuilt every time
        with open(out, "w", encoding="utf-8") as f:
            f.write(patched)
    print("fnet_dhcp_hostname: FNET DHCP client patched to send the host name (option 12)")
    return lib_env.File(out)


env.AddBuildMiddleware(swap_dhcp_client, PATTERN)
