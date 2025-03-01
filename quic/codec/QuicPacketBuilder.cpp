/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/codec/QuicPacketBuilder.h>

#include <folly/Random.h>
#include <quic/codec/PacketNumber.h>

namespace quic {

template <typename BufOp = BufAppender>
PacketNumEncodingResult encodeLongHeaderHelper(
    const LongHeader& longHeader,
    BufOp& bufop,
    uint32_t& spaceCounter,
    PacketNum largestAckedPacketNum) {
  uint8_t initialByte = kHeaderFormMask | LongHeader::kFixedBitMask |
      (static_cast<uint8_t>(longHeader.getHeaderType())
       << LongHeader::kTypeShift);
  PacketNumEncodingResult encodedPacketNum = encodePacketNumber(
      longHeader.getPacketSequenceNum(), largestAckedPacketNum);
  initialByte &= ~LongHeader::kReservedBitsMask;
  initialByte |= (encodedPacketNum.length - 1);

  if (longHeader.getHeaderType() == LongHeader::Types::Retry) {
    initialByte &= 0xF0;
    auto odcidSize = longHeader.getOriginalDstConnId()->size();
    initialByte |= (odcidSize == 0 ? 0 : odcidSize - 3);
  }

  bufop.template writeBE<uint8_t>(initialByte);
  bool isInitial = longHeader.getHeaderType() == LongHeader::Types::Initial;
  uint64_t tokenHeaderLength = 0;
  const std::string& token = longHeader.getToken();
  if (isInitial) {
    uint64_t tokenLength = token.size();
    QuicInteger tokenLengthInt(tokenLength);
    tokenHeaderLength = tokenLengthInt.getSize() + tokenLength;
  }
  auto longHeaderSize = sizeof(uint8_t) /* initialByte */ +
      sizeof(QuicVersionType) + sizeof(uint8_t) +
      longHeader.getSourceConnId().size() + sizeof(uint8_t) +
      longHeader.getDestinationConnId().size() + tokenHeaderLength +
      kMaxPacketLenSize + encodedPacketNum.length;
  if (spaceCounter < longHeaderSize) {
    spaceCounter = 0;
  } else {
    spaceCounter -= longHeaderSize;
  }
  bufop.template writeBE<uint32_t>(
      folly::to<uint32_t>(longHeader.getVersion()));
  bufop.template writeBE<uint8_t>(longHeader.getDestinationConnId().size());
  bufop.push(
      longHeader.getDestinationConnId().data(),
      longHeader.getDestinationConnId().size());
  bufop.template writeBE<uint8_t>(longHeader.getSourceConnId().size());
  bufop.push(
      longHeader.getSourceConnId().data(), longHeader.getSourceConnId().size());

  if (isInitial) {
    uint64_t tokenLength = token.size();
    QuicInteger tokenLengthInt(tokenLength);
    tokenLengthInt.encode([&](auto val) { bufop.writeBE(val); });
    if (tokenLength > 0) {
      bufop.push((const uint8_t*)token.data(), token.size());
    }
  }

  if (longHeader.getHeaderType() == LongHeader::Types::Retry) {
    auto& originalDstConnId = longHeader.getOriginalDstConnId();
    bufop.template writeBE<uint8_t>(longHeader.getOriginalDstConnId()->size());
    bufop.push(originalDstConnId->data(), originalDstConnId->size());

    // Write the retry token
    CHECK(!token.empty()) << "Retry packet must contain a token";
    bufop.push((const uint8_t*)token.data(), token.size());
  }
  // defer write of the packet num and length till payload has been computed
  return encodedPacketNum;
}

template <typename BufOp = BufAppender>
folly::Optional<PacketNumEncodingResult> encodeShortHeaderHelper(
    const ShortHeader& shortHeader,
    BufOp& bufop,
    uint32_t& spaceCounter,
    PacketNum largestAckedPacketNum) {
  auto packetNumberEncoding = encodePacketNumber(
      shortHeader.getPacketSequenceNum(), largestAckedPacketNum);
  if (spaceCounter <
      1U + packetNumberEncoding.length + shortHeader.getConnectionId().size()) {
    spaceCounter = 0;
    return folly::none;
  }
  uint8_t initialByte =
      ShortHeader::kFixedBitMask | (packetNumberEncoding.length - 1);
  initialByte &= ~ShortHeader::kReservedBitsMask;
  if (shortHeader.getProtectionType() == ProtectionType::KeyPhaseOne) {
    initialByte |= ShortHeader::kKeyPhaseMask;
  }
  bufop.template writeBE<uint8_t>(initialByte);
  --spaceCounter;

  bufop.push(
      shortHeader.getConnectionId().data(),
      shortHeader.getConnectionId().size());
  spaceCounter -= shortHeader.getConnectionId().size();
  return packetNumberEncoding;
}

RegularQuicPacketBuilder::RegularQuicPacketBuilder(
    uint32_t remainingBytes,
    PacketHeader header,
    PacketNum largestAckedPacketNum)
    : remainingBytes_(remainingBytes),
      packet_(std::move(header)),
      header_(folly::IOBuf::create(kLongHeaderHeaderSize)),
      body_(folly::IOBuf::create(kAppenderGrowthSize)),
      headerAppender_(header_.get(), kLongHeaderHeaderSize),
      bodyAppender_(body_.get(), kAppenderGrowthSize) {
  writeHeaderBytes(largestAckedPacketNum);
}

uint32_t RegularQuicPacketBuilder::getHeaderBytes() const {
  bool isLongHeader = packet_.header.getHeaderForm() == HeaderForm::Long;
  CHECK(packetNumberEncoding_)
      << "packetNumberEncoding_ should be valid after ctor";
  return folly::to<uint32_t>(header_->computeChainDataLength()) +
      (isLongHeader ? packetNumberEncoding_->length + kMaxPacketLenSize : 0);
}

uint32_t RegularQuicPacketBuilder::remainingSpaceInPkt() const {
  return remainingBytes_;
}

void RegularQuicPacketBuilder::writeBE(uint8_t data) {
  bodyAppender_.writeBE<uint8_t>(data);
  remainingBytes_ -= sizeof(data);
}

void RegularQuicPacketBuilder::writeBE(uint16_t data) {
  bodyAppender_.writeBE<uint16_t>(data);
  remainingBytes_ -= sizeof(data);
}

void RegularQuicPacketBuilder::writeBE(uint64_t data) {
  bodyAppender_.writeBE<uint64_t>(data);
  remainingBytes_ -= sizeof(data);
}

void RegularQuicPacketBuilder::write(const QuicInteger& quicInteger) {
  remainingBytes_ -=
      quicInteger.encode([&](auto val) { bodyAppender_.writeBE(val); });
}

void RegularQuicPacketBuilder::appendBytes(
    PacketNum value,
    uint8_t byteNumber) {
  appendBytes(bodyAppender_, value, byteNumber);
}

void RegularQuicPacketBuilder::appendBytes(
    BufAppender& appender,
    PacketNum value,
    uint8_t byteNumber) {
  auto bigValue = folly::Endian::big(value);
  appender.push(
      (uint8_t*)&bigValue + sizeof(bigValue) - byteNumber, byteNumber);
  remainingBytes_ -= byteNumber;
}

void RegularQuicPacketBuilder::insert(std::unique_ptr<folly::IOBuf> buf) {
  remainingBytes_ -= buf->computeChainDataLength();
  bodyAppender_.insert(std::move(buf));
}

void RegularQuicPacketBuilder::insert(
    std::unique_ptr<folly::IOBuf> buf,
    size_t limit) {
  std::unique_ptr<folly::IOBuf> streamData;
  folly::io::Cursor cursor(buf.get());
  cursor.clone(streamData, limit);
  // reminaingBytes_ update is taken care of inside this insert call:
  insert(std::move(streamData));
}

void RegularQuicPacketBuilder::insert(const BufQueue& buf, size_t limit) {
  std::unique_ptr<folly::IOBuf> streamData;
  folly::io::Cursor cursor(buf.front());
  cursor.clone(streamData, limit);
  // reminaingBytes_ update is taken care of inside this insert call:
  insert(std::move(streamData));
}

void RegularQuicPacketBuilder::appendFrame(QuicWriteFrame frame) {
  packet_.frames.push_back(std::move(frame));
}

RegularQuicPacketBuilder::Packet RegularQuicPacketBuilder::buildPacket() && {
  // at this point everything should been set in the packet_
  LongHeader* longHeader = packet_.header.asLong();
  size_t minBodySize = kMaxPacketNumEncodingSize -
      packetNumberEncoding_->length + sizeof(Sample);
  size_t extraDataWritten = 0;
  size_t bodyLength = body_->computeChainDataLength();
  while (bodyLength + extraDataWritten + cipherOverhead_ < minBodySize &&
         !packet_.frames.empty() && remainingBytes_ > kMaxPacketLenSize) {
    // We can add padding frames, but we don't need to store them.
    QuicInteger paddingType(static_cast<uint8_t>(FrameType::PADDING));
    write(paddingType);
    extraDataWritten++;
  }
  if (longHeader && longHeader->getHeaderType() != LongHeader::Types::Retry) {
    QuicInteger pktLen(
        packetNumberEncoding_->length + body_->computeChainDataLength() +
        cipherOverhead_);
    pktLen.encode([&](auto val) { headerAppender_.writeBE(val); });
    appendBytes(
        headerAppender_,
        packetNumberEncoding_->result,
        packetNumberEncoding_->length);
  }
  return Packet(std::move(packet_), std::move(header_), std::move(body_));
}

void RegularQuicPacketBuilder::writeHeaderBytes(
    PacketNum largestAckedPacketNum) {
  if (packet_.header.getHeaderForm() == HeaderForm::Long) {
    LongHeader& longHeader = *packet_.header.asLong();
    encodeLongHeader(longHeader, largestAckedPacketNum);
  } else {
    ShortHeader& shortHeader = *packet_.header.asShort();
    encodeShortHeader(shortHeader, largestAckedPacketNum);
  }
}

void RegularQuicPacketBuilder::encodeLongHeader(
    const LongHeader& longHeader,
    PacketNum largestAckedPacketNum) {
  packetNumberEncoding_ = encodeLongHeaderHelper(
      longHeader, headerAppender_, remainingBytes_, largestAckedPacketNum);
}

void RegularQuicPacketBuilder::encodeShortHeader(
    const ShortHeader& shortHeader,
    PacketNum largestAckedPacketNum) {
  packetNumberEncoding_ = encodeShortHeaderHelper(
      shortHeader, headerAppender_, remainingBytes_, largestAckedPacketNum);
  if (packetNumberEncoding_) {
    RegularQuicPacketBuilder::appendBytes(
        headerAppender_,
        packetNumberEncoding_->result,
        packetNumberEncoding_->length);
  }
}

void RegularQuicPacketBuilder::push(const uint8_t* data, size_t len) {
  bodyAppender_.push(data, len);
  remainingBytes_ -= len;
}

bool RegularQuicPacketBuilder::canBuildPacket() const noexcept {
  return remainingBytes_ != 0;
}

const PacketHeader& RegularQuicPacketBuilder::getPacketHeader() const {
  return packet_.header;
}

void RegularQuicPacketBuilder::setCipherOverhead(uint8_t overhead) noexcept {
  cipherOverhead_ = overhead;
}

StatelessResetPacketBuilder::StatelessResetPacketBuilder(
    uint16_t maxPacketSize,
    const StatelessResetToken& resetToken)
    : data_(folly::IOBuf::create(kAppenderGrowthSize)) {
  BufAppender appender(data_.get(), kAppenderGrowthSize);
  // TODO: randomize the length
  uint16_t randomOctetLength = maxPacketSize - resetToken.size() - 1;
  uint8_t initialByte = ShortHeader::kFixedBitMask;
  appender.writeBE<uint8_t>(initialByte);
  auto randomOctets = folly::IOBuf::create(randomOctetLength);
  folly::Random::secureRandom(randomOctets->writableData(), randomOctetLength);
  appender.push(randomOctets->data(), randomOctetLength);
  appender.push(resetToken.data(), resetToken.size());
}

Buf StatelessResetPacketBuilder::buildPacket() && {
  return std::move(data_);
}

VersionNegotiationPacketBuilder::VersionNegotiationPacketBuilder(
    ConnectionId sourceConnectionId,
    ConnectionId destinationConnectionId,
    const std::vector<QuicVersion>& versions)
    : remainingBytes_(kDefaultUDPSendPacketLen),
      packet_(
          generateRandomPacketType(),
          sourceConnectionId,
          destinationConnectionId),
      data_(folly::IOBuf::create(kAppenderGrowthSize)) {
  writeVersionNegotiationPacket(versions);
}

uint32_t VersionNegotiationPacketBuilder::remainingSpaceInPkt() {
  return remainingBytes_;
}

std::pair<VersionNegotiationPacket, Buf>
VersionNegotiationPacketBuilder::buildPacket() && {
  return std::make_pair<VersionNegotiationPacket, Buf>(
      std::move(packet_), std::move(data_));
}

void VersionNegotiationPacketBuilder::writeVersionNegotiationPacket(
    const std::vector<QuicVersion>& versions) {
  // Write header
  BufAppender appender(data_.get(), kAppenderGrowthSize);
  appender.writeBE<decltype(packet_.packetType)>(packet_.packetType);
  remainingBytes_ -= sizeof(decltype(packet_.packetType));
  appender.writeBE(
      static_cast<QuicVersionType>(QuicVersion::VERSION_NEGOTIATION));
  remainingBytes_ -= sizeof(QuicVersionType);
  appender.writeBE<uint8_t>(packet_.destinationConnectionId.size());
  remainingBytes_ -= sizeof(uint8_t);
  appender.push(
      packet_.destinationConnectionId.data(),
      packet_.destinationConnectionId.size());
  remainingBytes_ -= packet_.destinationConnectionId.size();
  appender.writeBE<uint8_t>(packet_.sourceConnectionId.size());
  remainingBytes_ -= sizeof(uint8_t);
  appender.push(
      packet_.sourceConnectionId.data(), packet_.sourceConnectionId.size());
  remainingBytes_ -= packet_.sourceConnectionId.size();
  // Write versions
  for (auto version : versions) {
    if (remainingBytes_ < sizeof(QuicVersionType)) {
      break;
    }
    appender.writeBE<QuicVersionType>(static_cast<QuicVersionType>(version));
    remainingBytes_ -= sizeof(QuicVersionType);
    packet_.versions.push_back(version);
  }
}

uint8_t VersionNegotiationPacketBuilder::generateRandomPacketType() const {
  // TODO: change this back to generating random packet type after we rollout
  // draft-13. For now the 0 packet type will make sure that the version
  // negotiation packet is not interpreted as a long header.
  // folly::Random::secureRandom<decltype(packet_.packetType)>();
  return kHeaderFormMask;
}

bool VersionNegotiationPacketBuilder::canBuildPacket() const noexcept {
  return remainingBytes_ != 0;
}

InplaceQuicPacketBuilder::InplaceQuicPacketBuilder(
    folly::IOBuf& iobuf,
    uint32_t remainingBytes,
    PacketHeader header,
    PacketNum largestAckedPacketNum)
    : iobuf_(iobuf),
      bufWriter_(iobuf, remainingBytes),
      remainingBytes_(remainingBytes),
      packet_(std::move(header)) {
  if (packet_.header.getHeaderForm() == HeaderForm::Long) {
    LongHeader& longHeader = *packet_.header.asLong();
    packetNumberEncoding_ = encodeLongHeaderHelper(
        longHeader, bufWriter_, remainingBytes, largestAckedPacketNum);
    if (longHeader.getHeaderType() != LongHeader::Types::Retry) {
      // Remember the position to write packet number and packet length.
      packetLenOffset_ = iobuf_.length();
      // With this builder, we will have to always use kMaxPacketLenSize to
      // write packet length.
      packetNumOffset_ = packetLenOffset_ + kMaxPacketLenSize;
      // Inside BufWriter, we already countde the packet len and packet number
      // bytes as written. Note that remainingBytes_ also already counted them.
      bufWriter_.append(packetNumberEncoding_->length + kMaxPacketLenSize);
    }
  } else {
    ShortHeader& shortHeader = *packet_.header.asShort();
    packetNumberEncoding_ = encodeShortHeaderHelper(
        shortHeader, bufWriter_, remainingBytes_, largestAckedPacketNum);
    if (packetNumberEncoding_) {
      appendBytes(
          bufWriter_,
          packetNumberEncoding_->result,
          packetNumberEncoding_->length);
    }
  }
  bodyStart_ = iobuf_.writableTail();
}

uint32_t InplaceQuicPacketBuilder::remainingSpaceInPkt() const {
  return remainingBytes_;
}

void InplaceQuicPacketBuilder::writeBE(uint8_t data) {
  bufWriter_.writeBE<uint8_t>(data);
  remainingBytes_ -= sizeof(data);
}

void InplaceQuicPacketBuilder::writeBE(uint16_t data) {
  bufWriter_.writeBE<uint16_t>(data);
  remainingBytes_ -= sizeof(data);
}

void InplaceQuicPacketBuilder::writeBE(uint64_t data) {
  bufWriter_.writeBE<uint64_t>(data);
  remainingBytes_ -= sizeof(data);
}

void InplaceQuicPacketBuilder::write(const QuicInteger& quicInteger) {
  remainingBytes_ -=
      quicInteger.encode([&](auto val) { bufWriter_.writeBE(val); });
}

void InplaceQuicPacketBuilder::appendBytes(
    PacketNum value,
    uint8_t byteNumber) {
  appendBytes(bufWriter_, value, byteNumber);
}

void InplaceQuicPacketBuilder::appendBytes(
    BufWriter& bufWriter,
    PacketNum value,
    uint8_t byteNumber) {
  auto bigValue = folly::Endian::big(value);
  bufWriter.push(
      (uint8_t*)&bigValue + sizeof(bigValue) - byteNumber, byteNumber);
  remainingBytes_ -= byteNumber;
}

void InplaceQuicPacketBuilder::insert(std::unique_ptr<folly::IOBuf> buf) {
  remainingBytes_ -= buf->computeChainDataLength();
  bufWriter_.insert(buf.get());
}

void InplaceQuicPacketBuilder::insert(
    std::unique_ptr<folly::IOBuf> buf,
    size_t limit) {
  remainingBytes_ -= limit;
  bufWriter_.insert(buf.get(), limit);
}

void InplaceQuicPacketBuilder::insert(const BufQueue& buf, size_t limit) {
  remainingBytes_ -= limit;
  bufWriter_.insert(buf.front(), limit);
}

void InplaceQuicPacketBuilder::appendFrame(QuicWriteFrame frame) {
  packet_.frames.push_back(std::move(frame));
}

const PacketHeader& InplaceQuicPacketBuilder::getPacketHeader() const {
  return packet_.header;
}

PacketBuilderInterface::Packet InplaceQuicPacketBuilder::buildPacket() && {
  LongHeader* longHeader = packet_.header.asLong();
  size_t minBodySize = kMaxPacketNumEncodingSize -
      packetNumberEncoding_->length + sizeof(Sample);
  size_t extraDataWritten = 0;
  size_t bodyLength = iobuf_.writableTail() - bodyStart_;
  while (bodyLength + extraDataWritten + cipherOverhead_ < minBodySize &&
         !packet_.frames.empty() && remainingBytes_ > kMaxPacketLenSize) {
    // We can add padding frames, but we don't need to store them.
    QuicInteger paddingType(static_cast<uint8_t>(FrameType::PADDING));
    write(paddingType);
    extraDataWritten++;
  }
  if (longHeader && longHeader->getHeaderType() != LongHeader::Types::Retry) {
    QuicInteger pktLen(
        packetNumberEncoding_->length + bodyLength + cipherOverhead_);
    pktLen.encode(
        [&](auto val) {
          auto bigEndian = folly::Endian::big(val);
          CHECK_EQ(sizeof(bigEndian), kMaxPacketLenSize);
          bufWriter_.backFill(
              (uint8_t*)&bigEndian, kMaxPacketLenSize, packetLenOffset_);
        },
        kMaxPacketLenSize);
    auto bigPacketNum = folly::Endian::big(packetNumberEncoding_->result);
    CHECK_GE(sizeof(bigPacketNum), packetNumberEncoding_->length);
    bufWriter_.backFill(
        (uint8_t*)&bigPacketNum + sizeof(bigPacketNum) -
            packetNumberEncoding_->length,
        packetNumberEncoding_->length,
        packetNumOffset_);
  }
  CHECK(
      !bodyStart_ ||
      (bodyStart_ >= iobuf_.data() && bodyStart_ <= iobuf_.tail()));
  // TODO: Get rid of these two wrapBuffer when Fizz::AEAD has a new interface
  // for encryption.
  return PacketBuilderInterface::Packet(
      std::move(packet_),
      (bodyStart_ ? folly::IOBuf::wrapBuffer(
                        iobuf_.data(), (bodyStart_ - iobuf_.data()))
                  : nullptr),
      (bodyStart_
           ? folly::IOBuf::wrapBuffer(bodyStart_, iobuf_.tail() - bodyStart_)
           : nullptr));
}

void InplaceQuicPacketBuilder::setCipherOverhead(uint8_t overhead) noexcept {
  cipherOverhead_ = overhead;
}

void InplaceQuicPacketBuilder::push(const uint8_t* data, size_t len) {
  bufWriter_.push(data, len);
  remainingBytes_ -= len;
}

bool InplaceQuicPacketBuilder::canBuildPacket() const noexcept {
  return remainingBytes_ != 0;
}

uint32_t InplaceQuicPacketBuilder::getHeaderBytes() const {
  bool isLongHeader = packet_.header.getHeaderForm() == HeaderForm::Long;
  CHECK(packetNumberEncoding_)
      << "packetNumberEncoding_ should be valid after ctor";
  return folly::to<uint32_t>(bodyStart_ - iobuf_.data()) +
      (isLongHeader ? packetNumberEncoding_->length + kMaxPacketLenSize : 0);
}

} // namespace quic
