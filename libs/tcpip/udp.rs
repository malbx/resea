use resea::collections::Vec;
use resea::collections::VecDeque;
use resea::std::boxed::Box;
use resea::std::cell::RefCell;
use resea::std::rc::Rc;
use resea::std::mem::size_of;
use crate::Result;
use crate::mbuf::Mbuf;
use crate::packet::Packet;
use crate::ip::IpAddr;
use crate::tcp::TcpSocket;
use crate::transport::{
    Socket, BindTo, Port, TransportProtocol, TransportHeader, UdpTransportHeader
};
use crate::endian::NetEndian;

struct TxPacket {
    dst_addr: IpAddr,
    dst_port: Port,
    payload: Vec<u8>,
}

struct RxPacket {
    src_addr: IpAddr,
    src_port: Port,
    payload: Vec<u8>,
}

pub struct UdpSocket {
    bind_to: BindTo,
    port: Port,
    tx: VecDeque<TxPacket>,
    rx: VecDeque<RxPacket>,
}

impl Socket for UdpSocket {
    fn build(&mut self) -> Option<(IpAddr, Mbuf)> {
        let tx = match self.tx.pop_front() {
            Some(tx) => tx,
            None => return None,
        };

        let mut mbuf = Mbuf::new();
        mbuf.prepend(&UdpHeader {
            dst_port: tx.dst_port.as_u16().into(),
            src_port: self.port.as_u16().into(),
            len: (tx.payload.len() as u16).into(),
            checksum: 0.into(),
        });
        mbuf.append_bytes(tx.payload.as_slice());

        Some((tx.dst_addr, mbuf))
    }

    fn receive<'a>(&mut self, src_addr: IpAddr, header: &TransportHeader<'a>) {
        let header = match header {
            TransportHeader::Udp(header) => header,
            _ => unreachable!()
        };
        
        let rx_data = RxPacket {
            src_addr,
            src_port: header.src_port,
            payload: Vec::from(header.payload),
        };

        self.rx.push_back(rx_data);
    }

    fn close(&mut self) {
        unreachable!();
    }

    fn read(&mut self, buf: &mut Vec<u8>, len: usize) -> usize {
        unreachable!();
    }

    fn write(&mut self, data: &[u8]) -> Result<()> {
        unreachable!();
    }

    fn accept(&mut self) -> Option<TcpSocket> {
        unreachable!();
    }

    fn protocol(&self) -> TransportProtocol {
        TransportProtocol::Udp
    }

    fn bind_to(&self) -> &BindTo {
        &self.bind_to
    }
}

#[repr(C, packed)]
struct UdpHeader {
    src_port: NetEndian<u16>,
    dst_port: NetEndian<u16>,
    len: NetEndian<u16>,
    checksum: NetEndian<u16>,
}

pub fn parse<'a>(pkt: &'a mut Packet) -> Option<(Port, Port, TransportHeader<'a>)> {
    let header = match pkt.consume::<UdpHeader>() {
        Some(header) => header,
        None => return None,
    };

    let dst_port = Port::new(header.dst_port.into());
    let src_port = Port::new(header.src_port.into());
    let total_len: u16 = header.len.into();
    let payload_len = (total_len as usize) - size_of::<UdpHeader>();
    let payload = unwrap_or_return!(pkt.consume_bytes(payload_len), None);

    let parsed_header = TransportHeader::Udp(UdpTransportHeader {
        src_port,
        payload,
    });

    Some((src_port, dst_port, parsed_header))
}