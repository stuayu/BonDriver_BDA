#include "common.h"

#include "crc32.h"

#include "TSMF.h"

CTSMFParser::CTSMFParser(void)
	: slot_counter(-1),
	TSID(0xffff),
	ONID(0xffff),
	IsRelative(FALSE),
	IsClearCalled(FALSE),
	PacketSize(0)
{
	::InitializeCriticalSection(&csClear);
}

CTSMFParser::~CTSMFParser(void)
{
	::DeleteCriticalSection(&csClear);
}

void CTSMFParser::SetTSID(WORD onid, WORD tsid, BOOL relative)
{
	Clear(onid, tsid, relative);
}

void CTSMFParser::Disable(void)
{
	Clear();
}

void CTSMFParser::ParseTsBuffer(BYTE * buf, size_t len, BYTE ** newBuf, size_t * newBufLen, BOOL deleteNullPackets)
{
	if (!buf || len <= 0)
		return;

	::EnterCriticalSection(&csClear);
	WORD onid = ONID;
	WORD tsid = TSID;
	BOOL relative = IsRelative;
	BOOL clearCalled = IsClearCalled;
	IsClearCalled = FALSE;
	::LeaveCriticalSection(&csClear);

	if (clearCalled) {
		// TSMF多重フレームの同期情報をクリア
		slot_counter = -1;

		// TSパケット同期情報をクリア
		PacketSize = 0;

		// 未処理TSパケットバッファをクリア
		readBuf.clear();

		// TLVパケットバッファをクリア
		tlvBuf.clear();
	}

	// 前回の残りデータと新規データを結合して新しいReadバッファを作成
	readBuf.insert(readBuf.end(), buf, buf + len);
	size_t readBufPos = 0;

	// Write用テンポラリバッファをクリア
	tempBuf.clear();

	// Readバッファを処理
	while (readBuf.size() - readBufPos > PacketSize) {
		if (PacketSize == 0) {
			// TSパケットの同期
			size_t truncate = 0;	// 切捨てサイズ
			SyncPacket(readBuf.data() + readBufPos, readBuf.size() - readBufPos, &truncate, &PacketSize);
			// TSパケット先頭までのデータを切り捨てる
			readBufPos += truncate;
			if (truncate == 0)
				// TSバッファのデータサイズが小さすぎて同期できない
				break;

			continue;
		}

		size_t tlv_headersize = 0;
		size_t tlv_start = 0;
		// 同期できている
		if (ParseOnePacket(readBuf.data() + readBufPos, readBuf.size() - readBufPos, onid, tsid, relative, &tlv_headersize, &tlv_start)) {
			WORD pid = ((readBuf[readBufPos + 1] << 8) | readBuf[readBufPos + 2]) & 0x1fff;
			if (!deleteNullPackets || pid != 0x1fff) {
				// 必要なTSMFフレームをテンポラリバッファへ追加
				if (tlv_headersize == 0) {
					// 通常のTSパケット
					tempBuf.insert(tempBuf.end(), readBuf.begin() + readBufPos, readBuf.begin() + readBufPos + PacketSize);
				}
				else if (tlv_start == 0) {
					// 先頭を含まない分割TLVパケットは前回分のバッファーと結合する
					if (!tlvBuf.empty())
						tlvBuf.insert(tlvBuf.end(), readBuf.begin() + readBufPos + tlv_headersize, readBuf.begin() + readBufPos + PacketSize);
				}
				else {
					// 先頭を含む分割TLVパケットの場合前回分のバッファーをtempBufに書き込んで新たな分割TLVバッファーを作成
					// 分割された状態のTLVパケットがtempBufに貯まらないようにしている
					if (!tlvBuf.empty()) {
						tlvBuf.insert(tlvBuf.end(), readBuf.begin() + readBufPos + tlv_headersize, readBuf.begin() + readBufPos + tlv_start);
						tempBuf.insert(tempBuf.end(), tlvBuf.begin(), tlvBuf.end());
						tlvBuf.clear();
					}
					tlvBuf.insert(tlvBuf.end(), readBuf.begin() + readBufPos + tlv_start, readBuf.begin() + readBufPos + PacketSize);
				}
			}
		}
		// 次のRead位置へ
		readBufPos += PacketSize;
	}

	// Write用テンポラリバッファが空でなければResult用バッファを作成
	if (!tempBuf.empty()) {
		::EnterCriticalSection(&csClear);
		clearCalled = IsClearCalled;
		::LeaveCriticalSection(&csClear);

		// 途中でクリアされたら捨てる
		if (!clearCalled) {
			BYTE * resultBuf = new BYTE[tempBuf.size()];
			memcpy(resultBuf, tempBuf.data(), tempBuf.size());
			*newBuf = resultBuf;
			*newBufLen = tempBuf.size();
		}
	}

	// 半端なTSデータが残っている場合は次回処理用に保存
	readBuf.erase(readBuf.begin(), readBuf.begin() + readBufPos);

	return;
}

void CTSMFParser::Clear(WORD onid, WORD tsid, BOOL relative)
{
	// ストリーム識別子を消去または変更
	::EnterCriticalSection(&csClear);
	ONID = onid;
	TSID = tsid;
	IsRelative = relative;
	IsClearCalled = TRUE;
	::LeaveCriticalSection(&csClear);
}

BOOL CTSMFParser::ParseTSMFHeader(const BYTE * buf, size_t len)
{
	static constexpr WORD FRAME_SYNC_MASK = 0x1fff;
	static constexpr WORD FRAME_SYNC_F = 0x1a86;
	static constexpr WORD FRAME_SYNC_I = ~FRAME_SYNC_F & FRAME_SYNC_MASK;

	// パケットサイズ
	if (len < 188)
		return FALSE;

	// 同期バイト
	BYTE sync_byte = buf[0];
	if (sync_byte != TS_PACKET_SYNC_BYTE)
		return FALSE;

	// 多重フレームPID
	WORD frame_PID = (buf[1] << 8) | buf[2];
	if (frame_PID != 0x002F)
		return FALSE;

	// 固定値
	if ((buf[3] & 0xf0) != 0x10)
		return FALSE;

	// 多重フレーム同期信号
	WORD frame_sync = ((buf[4] << 8) | buf[5]) & FRAME_SYNC_MASK;
	if (frame_sync != FRAME_SYNC_F && frame_sync != FRAME_SYNC_I)
		return FALSE;

	// CRC
	if (crc32(&buf[4], 184) != 0)
		return FALSE;

	// 連続性指標
	TSMFData.continuity_counter = buf[3] & 0x0f;

	// 変更指示
	TSMFData.version_number = (buf[6] & 0xE0) >> 5;

	// スロット配置法の区別
	TSMFData.relative_stream_number_mode = (buf[6] & 0x10) >> 4;
	if (TSMFData.relative_stream_number_mode != 0x0)
		return FALSE;

	// 多重フレーム形式
	TSMFData.frame_type = (buf[6] & 0x0f);
	if (TSMFData.frame_type != 0x1 && TSMFData.frame_type != 0x2)
		return FALSE;

	// 相対ストリーム番号毎の情報
	for (int i = 0; i < 15; i++) {
		// 相対ストリーム番号に対する有効、無効指示
		TSMFData.stream_info[i].stream_status = (buf[7 + (i / 8)] & (0x80 >> (i % 8))) >> (7 - (i % 8));
		// ストリーム識別／相対ストリーム番号対応情報
		TSMFData.stream_info[i].stream_id = (buf[9 + (i * 4)] << 8) | buf[10 + (i * 4)];
		// オリジナルネットワ−ク識別／相対ストリーム番号対応情報
		TSMFData.stream_info[i].original_network_id = (buf[11 + (i * 4)] << 8) | buf[12 + (i * 4)];
		// 受信状態
		TSMFData.stream_info[i].receive_status = (buf[69 + (i / 4)] & (0xc0 >> ((i % 4) * 2))) >> ((3 - (i % 4)) * 2);
		// ストリーム種別（拡張情報）
		TSMFData.stream_info[i].stream_type = (buf[125 + (i / 8)] & (0x80 >> (i % 8))) >> (7 - (i % 8));
	}

	// 緊急警報指示
	TSMFData.emergency_indicator = buf[72] & 0x01;

	// 相対ストリーム番号対スロット対応情報
	for (int i = 0; i < 52; i++) {
		TSMFData.relative_stream_number[i] = (buf[73 + (i / 2)] & (0xf0 >> ((i % 2) * 4))) >> ((1 - (i % 2)) * 4);
	}

	// 搬送波群の識別
	TSMFData.group_id = buf[127];

	// 搬送波の総数
	TSMFData.number_of_carriers = buf[128];

	// 搬送波の順序
	TSMFData.carrier_sequence = buf[129];

	// フレーム数
	TSMFData.number_of_frames = (buf[130] & 0xf0) >> 4;

	// フレーム位置情報
	TSMFData.frame_position = buf[130] & 0x0f;

	return TRUE;
}

/// <param name="tlv_headersize">分割TLVパケットの0x47を含めた先頭のヘッダーのサイズ</param>
/// <param name="tlv_start">**ヘッダーを含んだ**分割TLVパケットの先頭位置（先頭を含まないなら0）</param>
BOOL CTSMFParser::ParseOnePacket(const BYTE * buf, size_t len, WORD onid, WORD tsid, BOOL relative, size_t * tlv_headersize, size_t * tlv_start)
{
	if (!tlv_headersize || !tlv_start)
		return FALSE;

	*tlv_headersize = 0;
	*tlv_start = 0;

	if (buf[0] != TS_PACKET_SYNC_BYTE) {
		// TSパケットの同期外れ
		PacketSize = 0;
		slot_counter = -1;
		return FALSE;
	}

	if (tsid == 0xffff)
		// TSID指定が0xffffならば全てのスロットを返す
		return TRUE;

	if (ParseTSMFHeader(buf, len)) {
		// TSMF多重フレームヘッダ
		slot_counter = 0;
		return FALSE;
	}

	if (slot_counter < 0 || slot_counter > 51)
		// TSMF多重フレームの同期がとれていない
		return FALSE;

	slot_counter++;

	int ts_number = 0;
	if (relative) {
		// 相対TS番号を直接指定
		ts_number = (int)tsid + 1;
	}
	else {
		// ONIDとTSIDで指定
		for (int i = 0; i < 15; i++) {
			if (TSMFData.stream_info[i].stream_id == tsid && (onid == 0xffff || TSMFData.stream_info[i].original_network_id == onid)) {
				ts_number = i + 1;
				break;
			}
		}
	}
	if (ts_number < 1 || ts_number > 15)
		// 該当する相対TS番号が無い
		return FALSE;

	if (TSMFData.stream_info[ts_number - 1].stream_status == 0)
		// その相対TS番号は未使用
		return FALSE;

	if (TSMFData.relative_stream_number[slot_counter - 1] != ts_number)
		// このスロットは他の相対TS番号用スロットか未割当
		return FALSE;

	// ストリーム種別がTLVの場合はTLVパケットを出力する
	// トランスポートエラーインジケーターが0, 固定値が0, 分割TLVのPIDであるときは正常な分割TLVパケットと判断
	if (TSMFData.stream_info[ts_number - 1].stream_type == 0x0
		&& (((buf[1] >> 5) & 0b101) == 0b000)
		&& ((((buf[1] << 8) | buf[2]) & 0x1fff) == 0x2d)
		) {
		BYTE tlv_packet_start_indicator = (buf[1] >> 6) & 0x01;
		if (tlv_packet_start_indicator == 0x0) {
			*tlv_headersize = 3;
			*tlv_start = 0;
		}
		else {
			*tlv_headersize = 4;
			*tlv_start = buf[3] + 4;
		}
		if (*tlv_start > PacketSize) {
			return FALSE;
		}
	}
	return TRUE;
}

BOOL CTSMFParser::SyncPacket(const BYTE * buf, size_t len, size_t * truncate, size_t * packetSize)
{
	static size_t constexpr TS_PACKET_SEARCH_SIZE = 208;
	if (!truncate || !packetSize)
		return FALSE;

	*truncate = 0;
	*packetSize = 0;

	if (len < TS_PACKET_SEARCH_SIZE * 3 + 1)
		// TSデータが小さすぎて同期が取れない
		return FALSE;

	for (size_t i = 0; i < TS_PACKET_SEARCH_SIZE; i++) {
		if (buf[i] == TS_PACKET_SYNC_BYTE) {
			// 同期バイトが見つかった
			// 188バイトパケットの確認(通常のTSパケット)
			if (buf[i + 188] == TS_PACKET_SYNC_BYTE && buf[i + 188 * 2] == TS_PACKET_SYNC_BYTE) {
				*truncate = i;
				*packetSize = 188;
				return TRUE;
			}
			// 204バイトパケットの確認(FEC付)
			if (buf[i + 204] == TS_PACKET_SYNC_BYTE && buf[i + 204 * 2] == TS_PACKET_SYNC_BYTE) {
				*truncate = i;
				*packetSize = 204;
				return TRUE;
			}
			// 192バイトパケットの確認(タイムスタンプ付)
			if (buf[i + 192] == TS_PACKET_SYNC_BYTE && buf[i + 192 * 2] == TS_PACKET_SYNC_BYTE) {
				*truncate = i;
				*packetSize = 192;
				return TRUE;
			}
			// 208バイトパケットの確認(タイムスタンプ付)
			if (buf[i + 208] == TS_PACKET_SYNC_BYTE && buf[i + 208 * 2] == TS_PACKET_SYNC_BYTE) {
				*truncate = i;
				*packetSize = 208;
				return TRUE;
			}
		}
	}

	*truncate = TS_PACKET_SEARCH_SIZE;
	return FALSE;
}
