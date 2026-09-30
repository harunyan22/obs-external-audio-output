/*
Plugin Name
Copyright (C) 2026 minimamu

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// 対応 REQ: REQ-AUDIO-005, REQ-NFR-002 / Slice 3
//
// 水位ベースのドリフト補正ロジック(D-02 MVP)。純粋ロジックで libobs/Windows 非依存のため
// tests/unit から単体テストできる(ring-buffer.hpp と同方針)。WasapiOutput::RenderLoop
// (render thread)が毎 wake(WaitForSingleObject の起床ごと)OnWake() を呼び、RingBuffer の
// 現在水位を「frames 実効水位」(呼び出し側が Level()*frames_per_chunk + carry 残り frames で
// 計算する。レビュー指摘 H-1)で受け取り、「今回の読み出しで挿入(+1 frame)/間引き(-1 frame)/
// なにもしない」を決定する。ヒープ確保・ロックなしで動作する(RT 安全、rt-audio.md 準拠)。
// OnWake() は決定のみを返し、実際に統計へ計上する(NotifyApplied())のは RenderLoop が本当に
// 補正を適用できた時点(レビュー指摘 H-2: 適用できなかった wake は計上しない・次 wake で再挑戦)。
// EMA/モード/レート制限の内部状態は render thread だけが読み書きする(RenderLoop に閉じる)。
// 挿入/間引きの累計統計と EMA 値だけは atomic で公開し、診断スレッド(RT ではない)からの
// 読み取りを許容する。
//
// Slice 6(S3 持ち越し、ブリーフ実装内容 13): |EMA - target| が「2 チャンク相当」(2*frames_per_chunk)を
// 超えている間は「高速復帰モード」とし、補正レート上限を通常の 1/500 frames から 1/200 frames へ
// 引き上げる(帯域内に戻ったら 1/500 へ戻す)。Recovering からの再開直後など水位が目標から大きく
// 外れている状況を素早く解消するための拡張で、通常運用時(|EMA-target| が発動閾値程度)は
// 2*frames_per_chunk よりずっと小さいため、この切替は従来のヒステリシス挙動に影響しない
// (tests/unit/test-drift-compensator.cpp の既存ケースは |EMA-target| が常に frames_per_chunk 未満。
// レビュー指摘 Medium-8: 発動閾値 1→2 チャンク相当、レート上限 100→200 frames へそれぞれ緩和)。
// 高速モード中の適用は inserted_frames_total_/dropped_frames_total_ とは別の
// recovery_frames_total_ へ計上する(「drift_ins/drop は定常、drift_recovery_frames は高速モード分」)。

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

class DriftCompensator {
public:
	enum class Decision { None, Insert, Drop };

	// ヒステリシスモード(テスト/診断からも参照するため public)。Normal(補正なし)/
	// Drop(間引きモード)/Insert(挿入モード)。
	enum class Mode { Normal, Drop, Insert };

	// 補正レート上限: 最大 1 frame の補正 / kMinFramesBetweenCorrectionsNormal frames
	// (ブリーフ実装内容 3、≒0.2%、可聴化防止)。モード中でもこの上限は厳守する。
	static constexpr uint32_t kMinFramesBetweenCorrectionsNormal = 500;
	// Slice 6(S3 持ち越し、ブリーフ実装内容 13): 高速復帰モード中はレート上限を 1/200 frames へ
	// 引き上げる(可聴化のリスクより早期復帰を優先する。レビュー指摘 Medium-8: 100→200 へ緩和)。
	static constexpr uint32_t kMinFramesBetweenCorrectionsFastRecovery = 200;

	// EMA の時定数(ブリーフ: 約 2 秒相当)。frames 進行量ベースの alpha に換算する
	// (ComputeTauFrames() 参照、レビュー指摘 Medium-1)。
	static constexpr double kEmaTimeConstantMs = 2000.0;

	// target_level_frames: 目標水位(frames 単位、実効水位ベース)。呼び出し側(OutputInstance)が
	//   kPrerollChunks * frames_per_chunk で計算する。
	// frames_per_chunk: 1 チャンクが占める frames 数(デバイスレート換算。OutputInstance の
	//   FramesPerChunkForDeviceRate() と同じ値)。発動閾値/解除閾値のサイズ決定に使う
	//   (レビュー指摘 H-1)。
	// capacity_frames: RingBuffer の総容量を frames 換算した値(ring_capacity_chunks *
	//   frames_per_chunk 相当)。発動閾値のクランプ(「発動閾値 < 容量-target かつ > 0」)に使う。
	// device_sample_rate_hz: EMA の時定数(kEmaTimeConstantMs)を「1 frame あたりの重み」に
	//   換算する(ComputeTauFrames())ために使う。OnWake() は毎回 frames_this_wake(その wake で
	//   実際に処理された frames 数)を使って per-call の alpha を都度算出するため、固定の wake
	//   間隔(旧: wake_interval_ms の実測/近似値)を仮定しない(レビュー指摘 Medium-1)。
	DriftCompensator(double target_level_frames, double frames_per_chunk, double capacity_frames,
			 double device_sample_rate_hz)
		: target_level_frames_(target_level_frames),
		  frames_per_chunk_(frames_per_chunk),
		  trigger_threshold_frames_(
			  ComputeTriggerThreshold(frames_per_chunk, target_level_frames, capacity_frames)),
		  release_threshold_frames_(ComputeReleaseThreshold(frames_per_chunk, trigger_threshold_frames_)),
		  ema_tau_frames_(ComputeTauFrames(device_sample_rate_hz)),
		  ema_level_frames_(target_level_frames)
	{
	}

	DriftCompensator(const DriftCompensator &) = delete;
	DriftCompensator &operator=(const DriftCompensator &) = delete;

	// render thread から毎 wake(WaitForSingleObject の起床ごと)1 回だけ呼ぶ。
	// current_level_frames: 今回の wake で pop する前の frames 実効水位(呼び出し側が
	//   RingBuffer::Level()*frames_per_chunk + (carry_len_-carry_pos_) で計算する。レビュー指摘 H-1)。
	// frames_this_wake: 今回の wake で書き込む予定の frames 数(WASAPI の available。
	//   GetCurrentPadding から求めた available をそのまま渡す想定)。EMA の重み(alpha)算出と
	//   レート制限のカウントの両方に使う。
	// 戻り値: 今回適用が「可能」な補正の決定(まだ統計には計上しない)。呼び出し側が実際に
	// RingBuffer から pop したチャンクへ適用できたら NotifyApplied() を呼ぶこと(レビュー指摘 H-2)。
	// 適用できなかった場合は NotifyApplied() を呼ばない。frames_since_last_correction_ は
	// リセットされないままなので、次 wake の OnWake() は同じ決定を返し続ける(自動的に再挑戦になる)。
	// ヒープ確保・ロックなし(RT 安全)。
	Decision OnWake(double current_level_frames, uint32_t frames_this_wake)
	{
		// EMA 更新: frames_this_wake が実際に処理された frames 数そのものを重みに使う
		// (frames 進行量ベースの時定数、レビュー指摘 Medium-1)。frames_this_wake==0 の wake は
		// 実処理が無かったとみなし EMA を更新しない。
		if (frames_this_wake > 0) {
			double ema = ema_level_frames_.load(std::memory_order_relaxed);
			const double alpha = ema_tau_frames_ > 0.0
						     ? (1.0 - std::exp(-(double)frames_this_wake / ema_tau_frames_))
						     : 1.0; // tau のフォールバック: 即座に現在値へ追従。
			ema += alpha * (current_level_frames - ema);
			ema_level_frames_.store(ema, std::memory_order_relaxed);
		}

		const double ema_now = ema_level_frames_.load(std::memory_order_relaxed);

		// ヒステリシス: 目標 + 発動閾値 を超え続けたら間引きモード、目標 - 発動閾値 を下回り
		// 続けたら挿入モード、目標 ± 解除閾値 に戻ったら解除(レビュー指摘 H-1)。
		if (mode_ == Mode::Normal) {
			if (ema_now > target_level_frames_ + trigger_threshold_frames_)
				mode_ = Mode::Drop;
			else if (ema_now < target_level_frames_ - trigger_threshold_frames_)
				mode_ = Mode::Insert;
		} else if (mode_ == Mode::Drop) {
			if (ema_now <= target_level_frames_ + release_threshold_frames_)
				mode_ = Mode::Normal;
		} else { // Mode::Insert
			if (ema_now >= target_level_frames_ - release_threshold_frames_)
				mode_ = Mode::Normal;
		}

		// Slice 6(S3 持ち越し、ブリーフ実装内容 13、レビュー指摘 Medium-8): |EMA-target| が
		// 2 チャンク相当を超えている間は高速復帰モード(レート上限を 200 frames へ引き上げ)。
		// ヒステリシスの発動/解除閾値(frames_per_chunk/4 前後)よりずっと大きい
		// 2*frames_per_chunk_ を境目にしているため、通常のヒステリシス遷移(Normal⇔Drop/Insert)
		// には影響しない(クラス冒頭コメント参照)。
		fast_recovery_active_ = std::fabs(ema_now - target_level_frames_) > (2.0 * frames_per_chunk_);
		const uint32_t min_frames_between = fast_recovery_active_ ? kMinFramesBetweenCorrectionsFastRecovery
									  : kMinFramesBetweenCorrectionsNormal;

		// レート制限は「処理した frame 数」を内部でカウントし、上限に達するまでは None を返す
		// (ブリーフ実装内容 3)。モードに関係なくカウントは進める(モード解除直後に即座に補正が
		// 再開されて可聴化するのを避けるため、カウンタをモード切替でリセットしない)。リセットは
		// NotifyApplied() が実際に適用された時点でのみ行う(レビュー指摘 H-2)。
		frames_since_last_correction_ += frames_this_wake;
		// uint32 wrap 対策(再レビュー指摘 L): kMinFramesBetweenCorrectionsNormal(2 種のうち大きい方)
		// 以上はこれ以上大きくする意味がないので、加算のたびに頭打ちにしてオーバーフローを未然に防ぐ。
		// 高速復帰モードの小さい上限(100)との比較でも、頭打ち値(500)は常にそれ以上なので正しく動作する。
		if (frames_since_last_correction_ > kMinFramesBetweenCorrectionsNormal)
			frames_since_last_correction_ = kMinFramesBetweenCorrectionsNormal;

		if (mode_ == Mode::Normal)
			return Decision::None;
		if (frames_since_last_correction_ < min_frames_between)
			return Decision::None;

		return mode_ == Mode::Drop ? Decision::Drop : Decision::Insert;
	}

	// RenderLoop が OnWake() の決定を実際に(1 回だけ)適用できたときに呼ぶ。ここで初めて
	// frames_since_last_correction_ をリセットし、累計統計を加算する(レビュー指摘 H-2)。
	// decision が None のときは no-op。RT 安全(atomic 加算のみ)。
	// Slice 6: 直前の OnWake() が高速復帰モードで決定したものは inserted/dropped ではなく
	// recovery_frames_total_ へ計上する(「drift_ins/drop は定常、drift_recovery_frames は高速モード分」)。
	void NotifyApplied(Decision decision)
	{
		if (decision == Decision::None)
			return;

		frames_since_last_correction_ = 0;
		if (fast_recovery_active_) {
			recovery_frames_total_.fetch_add(1, std::memory_order_relaxed);
		} else if (decision == Decision::Drop) {
			dropped_frames_total_.fetch_add(1, std::memory_order_relaxed);
		} else {
			inserted_frames_total_.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// 診断/テスト用の内部状態読み取り。EMA 値は診断スレッド(RT ではない)からも読むため atomic。
	double EmaLevelFrames() const { return ema_level_frames_.load(std::memory_order_relaxed); }
	Mode CurrentMode() const { return mode_; }

	// 構築時に決定される閾値(frames)。診断/テストからの参照用(レビュー指摘 H-1)。
	double TriggerThresholdFrames() const { return trigger_threshold_frames_; }
	double ReleaseThresholdFrames() const { return release_threshold_frames_; }

	// 統計: 挿入/間引きの累計 frames(ブリーフ実装内容 5)。診断スレッドから読む。
	uint64_t InsertedFrames() const { return inserted_frames_total_.load(std::memory_order_relaxed); }
	uint64_t DroppedFrames() const { return dropped_frames_total_.load(std::memory_order_relaxed); }
	// Slice 6: 高速復帰モード中に適用された補正の累計 frames(挿入/間引きの内訳は問わない)。
	uint64_t RecoveryFrames() const { return recovery_frames_total_.load(std::memory_order_relaxed); }
	// 直近の OnWake() が高速復帰モードだったか(診断/テスト用)。
	bool IsFastRecoveryActive() const { return fast_recovery_active_; }

private:
	// 発動閾値(frames)。ブリーフ: max(frames_per_chunk/4, 128)。ただし構築時に
	// 「発動閾値 < 容量-target かつ > 0」を検証し、不成立なら警告ログ+補正無効化ではなく
	// 閾値そのものをクランプする(レビュー指摘 H-1)。
	static double ComputeTriggerThreshold(double frames_per_chunk, double target_level_frames,
					      double capacity_frames)
	{
		double threshold = frames_per_chunk / 4.0;
		if (threshold < 128.0)
			threshold = 128.0;

		const double max_allowed = capacity_frames - target_level_frames;
		if (max_allowed <= 0.0) {
			// 目標水位が容量以上という異常構成(実用上は起きない想定)。補正を無効化せず、
			// 「発動閾値 > 0」だけを維持する最小値へクランプする。
			return 1.0;
		}
		if (threshold >= max_allowed) {
			// 発動閾値が「容量 - 目標水位」以上だと、間引きモードの水位上限が容量を
			// 超えてしまい満杯検出ができなくなる。安全側(半分)へクランプする。
			threshold = max_allowed * 0.5;
		}
		return threshold > 0.0 ? threshold : 1.0;
	}

	// 解除閾値(frames)。ブリーフ: frames_per_chunk/8。発動閾値より小さくなるようクランプする
	// (ヒステリシスの順序を保証するため)。
	static double ComputeReleaseThreshold(double frames_per_chunk, double trigger_threshold_frames)
	{
		double release = frames_per_chunk / 8.0;
		if (release >= trigger_threshold_frames)
			release = trigger_threshold_frames * 0.5;
		return release >= 0.0 ? release : 0.0;
	}

	// EMA 時定数(kEmaTimeConstantMs)を frames 単位に換算する(離散 EMA: alpha = 1 -
	// exp(-frames_this_wake/tau_frames)。レビュー指摘 Medium-1)。
	static double ComputeTauFrames(double device_sample_rate_hz)
	{
		if (device_sample_rate_hz <= 0.0)
			return 0.0; // 異常値のフォールバック(OnWake 側で alpha=1.0 扱いになる)。
		return (kEmaTimeConstantMs / 1000.0) * device_sample_rate_hz;
	}

	const double target_level_frames_;
	// Slice 6: 高速復帰モードの切替閾値(|EMA-target| > frames_per_chunk_)に使う。構築時の値を
	// そのまま保持する(コンストラクタ引数の frames_per_chunk はローカルのままだったため、OnWake() で
	// 参照できるようメンバへ格納した)。
	const double frames_per_chunk_;
	const double trigger_threshold_frames_;
	const double release_threshold_frames_;
	const double ema_tau_frames_;

	// render thread のみが読み書きする(OnWake() は RenderLoop からのみ呼ばれる)。EMA 値だけは
	// 診断スレッドからも読むため atomic にしている(store/load は relaxed で十分。表示用途のため
	// 厳密な同期は不要)。
	std::atomic<double> ema_level_frames_;
	Mode mode_ = Mode::Normal;
	uint32_t frames_since_last_correction_ = 0;
	// Slice 6: 直近の OnWake() 呼び出し時点で高速復帰モードだったか(NotifyApplied() が統計の
	// 計上先を振り分けるのに使う。render thread のみが読み書き)。
	bool fast_recovery_active_ = false;

	std::atomic<uint64_t> inserted_frames_total_{0};
	std::atomic<uint64_t> dropped_frames_total_{0};
	std::atomic<uint64_t> recovery_frames_total_{0};
};
