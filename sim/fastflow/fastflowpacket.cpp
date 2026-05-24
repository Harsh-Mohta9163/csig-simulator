// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "fastflowpacket.h"

PacketDB<FastflowPacket> FastflowPacket::_packetdb;
PacketDB<FastflowAck>    FastflowAck::_packetdb;
PacketDB<FastflowPull>   FastflowPull::_packetdb;
