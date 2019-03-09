#!/usr/bin/env python3
import argparse
from pprint import pprint
from pathlib import Path
import sys
import jinja2
import colorama
from colorama import Fore, Style
from lib.idl import parse_idl, InvalidIdlException

TEMPLATE = """\
//! Generated by `idl.py`.
#![allow(clippy::all)]
#![allow(dead_code)]
#![allow(unused_imports)]
#![allow(unused_parens)]
#![allow(unused_variables)]
#![allow(non_camel_case_types)]

use crate::channel::{Channel, CId};
use crate::message::{Msg, Header, Page};
use crate::syscalls::Result;

pub const INTERFACE_ID: u8 = {{ interface_id }};

// Types
{% for t in types %}\
pub type {{ t.name }} = {{ t.alias_of }};
{% endfor %}\

// Message ID and headers
{% for m in messages %}\
{% if m.attrs["type"] in ["rpc", "upcall"] %}\
pub const {{ m.name | upper }}_REQUEST_ID: u16  = ((INTERFACE_ID as u16) << 8) | {{ m.attrs["id"] }};
pub const {{ m.name | upper }}_RESPONSE_ID: u16 = ((INTERFACE_ID as u16) << 8) | (1 << 7) | {{ m.attrs["id"] }};
pub const {{ m.name | upper }}_REQUEST_HEADER: usize =
    /* label */
    (({{ m.name | upper }}_REQUEST_ID as usize) << 16)
    /* # of page payloads */
    | ({{ m.req | page_payloads | count }} << 12)
    /* length of inline payloads */
    | (core::mem::size_of::<{{ m.name | req_msg_name }}>() - core::mem::size_of::<usize>() * 5)
    ;
pub const {{ m.name | upper }}_RESPONSE_HEADER: usize =
    /* label */
    (({{ m.name | upper }}_RESPONSE_ID as usize) << 16)
    /* # of page payloads */
    | ({{ m.res | page_payloads | count }} << 12)
    /* length of inline payloads */
    | (core::mem::size_of::<{{ m.name | res_msg_name }}>() - core::mem::size_of::<usize>() * 5)
    ;

#[repr(C, packed)]
pub struct {{ m.name | req_msg_name }} {
    pub header: Header,
    pub from: Channel,
{% for name in (m.req | page_payloads) %}\
    pub {{ name }}: Page,
{% endfor %}\
    __unused_page: [Page; {{ 3 - (m.req | page_payloads | count) }}],
{% for (name, type_) in (m.req | inline_payloads) %}\
    pub {{ name }}: {{ type_ | rename_type }},
{% endfor %}\
}

impl Msg for {{ m.name | req_msg_name }} {}

#[repr(C, packed)]
pub struct {{ m.name | res_msg_name }} {
    pub header: Header,
    pub from: Channel,
{% for name in (m.res | page_payloads) %}\
    pub {{ name }}: Page,
{% endfor %}\
    __unused_page: [Page; {{ 3 - (m.res | page_payloads | count) }}],
{% for (name, type_) in (m.res | inline_payloads) %}\
    pub {{ name }}: {{ type_ | rename_type }},
{% endfor %}\
}

impl Msg for {{ m.name | res_msg_name }} {}
{% endif %}\
{% endfor %}\

// Server trait
pub trait Server {
{% for m in messages %}\
{% if m.attrs["type"] == "rpc" %}\
    fn {{ m.name }}(&mut self{{ m.req | args_list }}) -> Result<({{ m.res | rets_list }})>;
{% endif %}\
{% endfor %}\
}

impl Server {
    pub fn handle(&mut self, server_ch: &Channel, header: Header) {
        match header.msg_id() {
{% for m in messages %}\
{% if m.attrs["type"] == "rpc" %}\
            {{ m.name | upper }}_REQUEST_ID => {
                let m: {{ m.name | req_msg_name }} = server_ch.read_buffer();
                let reply_to = m.from;
                match self.{{ m.name }}({{ m.req | cast_to_args }}) {
                    Ok(rets) => {
                        let r = {{ m.name | res_msg_name }} {
                            header: Header::from_usize({{ m.name | upper }}_RESPONSE_HEADER),
                            from: unsafe { core::mem::uninitialized() },
                            __unused_page: unsafe { core::mem::uninitialized() },\
                        {% if m.res | length == 1 %}
                            {{ m.res[0][0] }}: rets.into(),\
                        {% else %}
                            {% for (name, _) in m.res %}
                            {{ name }}: rets.{{ loop.index0 }}.into(),\
                            {% endfor %}
                        {% endif %}
                        };

                        if let Err(err) = reply_to.send(r) {
                            internal_println!("{}: warn: failed to reply: {:?}",
                                env!("PROGRAM_NAME"), err);
                        }
                    },
                    _ => unimplemented!(),
                }
            }
{% endif %}\
{% endfor %}\
            _ => {
                internal_println!("unknown method: {}", header.method_id());
            }
        }
    }
}

// Client
pub struct Client {
    server: Channel
}

impl Client {
    pub fn from_channel(ch: Channel) -> Client {
        Client { server: ch }
    }

    pub fn from_raw_cid(cid: isize) -> Client {
        Client { server: Channel::from_raw_cid(cid) }
    }

{% for m in messages %}\
{% if m.attrs["type"] == "rpc" %}\
    pub fn {{ m.name }}(&self{{ m.req | args_list }}) -> Result<{{ m.name | res_msg_name }}> {
        let __r = self.server.call(
            {{ m.name | camelcase }}Msg {
                header: Header::from_usize({{ m.name | upper }}_REQUEST_HEADER),
                from: unsafe { core::mem::uninitialized() },
                __unused_page: unsafe { core::mem::uninitialized() },\
            {% for (name, _) in m.req %}
                {{ name }}: {{ name }}.into(),\
            {% endfor %}
            }
        );

        __r
    }
{% endif %}\
{% endfor %}\
}
"""

def rename_type(name):
    rename_map = {
        "page": "Page",
    }

    return rename_map.get(name, name)

def args_list(args):
    s = ""
    for name, type_ in args:
        s += f", {name}: {rename_type(type_)}"
    return s

def rets_list(rets):
    values = []
    for _,type_ in rets:
        values.append(f"{rename_type(type_)}")
    return ", ".join(values)

def cast_to_args(payloads):
    args = []
    for name, _ in payloads:
        args.append(f"m.{name}.into()")
    return ", ".join(args)

def page_payloads(payloads):
    page_payloads = []
    for name, type_ in payloads:
        if type_ == "page":
            page_payloads.append(name)
    return page_payloads

def inline_payloads(payloads):
    inline_payloads = []
    for name, type_ in payloads:
        if type_ not in ["page"]:
            inline_payloads.append((name, type_))
    return inline_payloads

def camelcase(text):
    new_text = ""
    spans = text.split("_")
    for span in spans:
        new_text += span.capitalize()
    return new_text

TYPES = {}
def load_types(types):
    global TYPES
    for t in types:
        TYPES[t["name"]] = t["alias_of"]

INLINE_TYPES = ["usize", "isize", "u8", "char"]
def resolve_payload_type(t):
    while True:
        if t not in TYPES:
            break
        t = TYPES[t]

    if t in INLINE_TYPES:
        return "inline"

    if t == "page":
        return "page"

    raise InvalidIdlException(f"invalid type: `{t}'")

def generate_stub(idl):
    load_types(idl["types"])

    env = jinja2.Environment()
    env.filters["args_list"] = args_list
    env.filters["rets_list"] = rets_list
    env.filters["cast_to_args"] = cast_to_args
    env.filters["rename_type"] = rename_type
    env.filters["camelcase"] = camelcase
    env.filters["page_payloads"] = page_payloads
    env.filters["inline_payloads"] = inline_payloads
    env.filters["req_msg_name"] = lambda name: f"{camelcase(name)}Msg"
    env.filters["res_msg_name"] = lambda name: f"{camelcase(name)}ResponseMsg"

    return env.from_string(TEMPLATE).render(
        interface_id=idl["attrs"]["id"],
        interface_name=idl["name"],
        types=idl["types"],
        messages=idl["messages"],
    )

def main():
    parser = argparse.ArgumentParser(description="Resea Interface stub generator.")
    parser.add_argument("-o", dest="output", required=True, help="The output file.")
    parser.add_argument("idl_file", help="A idl file.")
    args = parser.parse_args()

    try:
        idl = parse_idl(open(args.idl_file).read())
        stub = generate_stub(idl)
    except InvalidIdlException as e:
        sys.exit(f"{Style.BRIGHT}idl.py: {Fore.RED}error:{Fore.RESET} {args.idl_file}: {e}")

    with open(args.output, "w") as f:
        f.write(stub)

if __name__ == "__main__":
    colorama.init(autoreset=True)
    main()
