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

// 対応 REQ: REQ-COMPAT-001 / Slice 9

#include "monitor-compat-controller.hpp"

#include "../audio/track-map.hpp"
#include "../device/audio-device-manager.hpp"
#include "../output/output-instance.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <windows.h>

#include <algorithm>

namespace {

// obs_module_config_path() のファイル名。config.json とは別ファイル(スキーマ変更不要、slice-9.md
// 実装変更範囲)。
constexpr const char *kRestoreFileName = "compat-restore.json";

// レビュー最終指摘(Medium-A): WriteRestoreFileIfMissing() が既存ファイルの parse に失敗したとき、
// 破損ファイルを退避する固定名(連番なし)。同名が既にあれば os_rename() が上書きする
// (MOVEFILE_REPLACE_EXISTING、platform-windows.c で確認済み)ため、直近 1 回分の破損ファイルだけ
// 保持する。
constexpr const char *kCorruptRestoreFileName = "compat-restore.json.corrupt.bak";

// 独立出力の Stop() 完了待ちの上限(REQ-COMPAT-001 制約 5、排他)。qt-ui-and-lifecycle.md の EXIT 待ち
// (OutputInstance::Shutdown())と同種の「UI スレッドを短時間ブロックする」設計判断。
// レビュー指摘(Medium-1): 元は 3000ms だったが、UI スレッドの Sleep ブロック時間を短縮するため
// 1500ms へ引き下げた。QCoreApplication::processEvents() による待ち中のポンプは、ユーザー操作が
// この Sleep ループの途中に割り込む再入危険があるため使わない。タイムアウト時は Enable() が false を
// 返し、呼び出し元(ExternalOutputDock)が output_mode を independent へ巻き戻す(レビュー指摘
// High-3)。Sleep ループそのものを無くす完全な非同期化(コマンドキュー越しに UI スレッドを
// ブロックしない設計)は S9 のスコープ外(将来の Slice で検討)。
constexpr int kStopWaitTimeoutMs = 1500;
constexpr int kStopWaitPollMs = 50;

// レビュー指摘(High-2a): Reapply() での obs_set_audio_monitoring_device 失敗時の指数バックオフ幅
// (2s→4s→8s→…上限30s)。
constexpr uint64_t kInitialDeviceApplyBackoffNs = 2000ULL * 1000000ULL;
constexpr uint64_t kMaxDeviceApplyBackoffNs = 30000ULL * 1000000ULL;

// レビュー指摘(Medium-F): GetCachedDefaultDeviceId() の時間ベース最低更新間隔(トポロジカウンタが
// 変化しない DeviceNotifier 登録失敗セッションでもキャッシュを腐らせないため)。
constexpr uint64_t kDefaultDeviceCacheMaxAgeNs = 30000ULL * 1000000ULL;

// レビュー指摘(High-C): compat-restore.json の復元を諦めるまでの試行回数上限。これを超えても
// missing/race が解消しない場合、未復元ソース名を列挙してファイルを削除する(恒久残存による
// obs_set_audio_monitoring_device の毎起動上書きを止める)。
constexpr int kMaxRestoreAttempts = 3;

compat_plan::MonitoringType ToCompatMonitoringType(obs_monitoring_type type)
{
	switch (type) {
	case OBS_MONITORING_TYPE_MONITOR_ONLY:
		return compat_plan::MonitoringType::MonitorOnly;
	case OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT:
		return compat_plan::MonitoringType::MonitorAndOutput;
	case OBS_MONITORING_TYPE_NONE:
	default:
		return compat_plan::MonitoringType::None;
	}
}

obs_monitoring_type ToObsMonitoringType(compat_plan::MonitoringType type)
{
	switch (type) {
	case compat_plan::MonitoringType::MonitorOnly:
		return OBS_MONITORING_TYPE_MONITOR_ONLY;
	case compat_plan::MonitoringType::MonitorAndOutput:
		return OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT;
	case compat_plan::MonitoringType::None:
	default:
		return OBS_MONITORING_TYPE_NONE;
	}
}

} // namespace

MonitorCompatController::MonitorCompatController(OutputInstance *output_instance) : output_instance_(output_instance) {}

std::vector<compat_plan::SourceInfo> MonitorCompatController::EnumerateSourceInfos() const
{
	// ARCHITECTURE.md §12 事実 3: obs_enum_sources/obs_source_get_audio_mixers/
	// obs_source_get_monitoring_type はいずれも公開 API。ここでは POD(名前・ビットマスク・列挙値)しか
	// 取り出さず、obs_source_t* を保持し続けないため obs_source_get_ref() は不要
	// (output-instance.cpp の FindMonitorOnlySources() は名前を列挙後も使うため ref を取っているが、
	// こちらはコールバック内で std::string へコピーし終える)。
	std::vector<compat_plan::SourceInfo> result;
	obs_enum_sources(
		[](void *param, obs_source_t *source) -> bool {
			auto *out = static_cast<std::vector<compat_plan::SourceInfo> *>(param);
			const char *name = obs_source_get_name(source);
			if (!name || name[0] == '\0')
				return true;

			compat_plan::SourceInfo info;
			info.name = name;
			info.mixers_bitmask = obs_source_get_audio_mixers(source);
			info.monitoring_type = ToCompatMonitoringType(obs_source_get_monitoring_type(source));
			info.has_do_not_self_monitor =
				(obs_source_get_output_flags(source) & OBS_SOURCE_DO_NOT_SELF_MONITOR) != 0;
			out->push_back(std::move(info));
			return true;
		},
		&result);
	return result;
}

void MonitorCompatController::ApplySourceChanges(const std::vector<std::string> &to_change) const
{
	for (const auto &name : to_change) {
		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (!source) {
			obs_log(LOG_WARNING,
				"MonitorCompatController: source \"%s\" disappeared before it could be "
				"switched to MONITOR_AND_OUTPUT",
				name.c_str());
			continue;
		}
		obs_source_set_monitoring_type(source, OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
		obs_source_release(source);
	}
}

std::string MonitorCompatController::ResolveDeviceDisplayName(const std::string &device_id,
							      const std::string &device_name) const
{
	if (!device_name.empty())
		return device_name;

	const AudioDeviceEnumeration enumeration = AudioDeviceManager::Enumerate();
	if (enumeration.succeeded) {
		if (device_id == "default" || device_id.empty()) {
			for (const auto &entry : enumeration.devices) {
				if (entry.id == enumeration.default_device_id)
					return entry.name;
			}
		} else {
			for (const auto &entry : enumeration.devices) {
				if (entry.id == device_id)
					return entry.name;
			}
		}
	}

	// obs_set_audio_monitoring_device は name が空文字列だと失敗する(S9-2)ため、最終手段として
	// device_id=="default" なら locale 済みの汎用文言(Dock の既定デバイス表示と同じキー)、それ以外は
	// device_id をそのまま名前として使う。
	if (device_id == "default" || device_id.empty()) {
		const char *fallback_name = obs_module_text("ExternalAudioOutput.Dock.DefaultDeviceOption");
		return fallback_name ? std::string(fallback_name) : std::string("Default Device");
	}
	return device_id;
}

std::string MonitorCompatController::GetCachedDefaultDeviceId(bool force_refresh)
{
	// レビュー指摘(High-2b): output_instance_->DeviceTopologyChangeCount() は control thread が
	// DeviceNotifier(DefaultDeviceChanged/DeviceArrived)を受信するたびに増分する atomic カウンタ
	// (output-instance.cpp の HandleDeviceEvent() 参照)。前回キャッシュ時から変化していなければ
	// AudioDeviceManager::Enumerate()(COM 列挙、Reapply() の 2 秒タイマーから見るとコストが高い)を
	// 呼ばずに再利用する。
	const uint64_t topology_count = output_instance_ ? output_instance_->DeviceTopologyChangeCount() : 0;
	// レビュー指摘(Medium-F): DeviceNotifier 登録に失敗したセッションは topology_count が永久に
	// 変化しないため、トポロジカウンタだけに頼るとキャッシュが無期限に古くなる。時間ベースの
	// 最低更新間隔(30 秒)を併用する。
	const uint64_t now_ns = os_gettime_ns();
	const bool cache_aged_out = default_device_id_cache_valid_ &&
				    (now_ns - default_device_id_cache_updated_at_ns_) >= kDefaultDeviceCacheMaxAgeNs;
	if (force_refresh || !default_device_id_cache_valid_ ||
	    topology_count != default_device_id_cache_topology_count_ || cache_aged_out) {
		const AudioDeviceEnumeration enumeration = AudioDeviceManager::Enumerate();
		default_device_id_cache_ = enumeration.succeeded ? enumeration.default_device_id : std::string();
		default_device_id_cache_topology_count_ = topology_count;
		default_device_id_cache_valid_ = true;
		default_device_id_cache_updated_at_ns_ = now_ns;
	}
	return default_device_id_cache_;
}

bool MonitorCompatController::Enable(int audio_track, const std::string &output_device_id,
				     const std::string &output_device_name)
{
	// レビュー指摘(Medium-D): デバイス変更後に旧バックオフ状態を引き継がない。Reapply() が積んだ
	// 「次回試行までの待ち時間」を Enable() のたびにリセットしないと、直後の obs_set_audio_monitoring_
	// device() 呼び出し(新デバイスへの初回反映)がここでは行われる一方、次の Reapply() の判定基準
	// (current_id != output_device_id_)自体は影響を受けないため実害は限定的だが、状態変化ログの
	// device_mismatch_last_logged_ が古いデバイス宛のまま残るのを防ぐ。
	device_apply_backoff_ns_ = 0;
	device_apply_retry_at_ns_ = 0;
	device_mismatch_last_logged_ = false;

	const int new_audio_track = audio_track_map::ClampAudioTrack(audio_track);

	if (enabled_) {
		if (new_audio_track == audio_track_ && output_device_id == output_device_id_) {
			// 既に有効・トラック/出力先とも変更なし: 再適用のみ(冪等)。
			output_device_name_ = ResolveDeviceDisplayName(output_device_id, output_device_name);
			Reapply();
			return true;
		}
		// レビュー3回目指摘(Medium-2): トラック/デバイスの変更は Reapply() だけでは旧トラックの
		// ソースが MONITOR_AND_OUTPUT のまま残ってしまう(Reapply() は新規出現ソースの追従のみ行い、
		// 対象から外れたソースを元に戻さないため、両トラックが鳴り続ける)。「Disable(非 EXIT、
		// 復元+成功時ファイル削除) → Enable(新設定)」を直列実行することで、旧トラックのソースを
		// 確実に元へ戻してから新トラックを適用する(シーンコレクション切替と同じパターン、
		// OnSceneCollectionChanging/Changed 参照)。
		obs_log(LOG_INFO,
			"MonitorCompatController: track/device change while enabled (track %d->%d, "
			"device_id \"%s\"->\"%s\"); disabling before re-enabling",
			audio_track_, new_audio_track, output_device_id_.c_str(), output_device_id.c_str());
		Disable(/*is_exit_context=*/false);
	}

	audio_track_ = new_audio_track;
	output_device_id_ = output_device_id;
	output_device_name_ = ResolveDeviceDisplayName(output_device_id, output_device_name);

	// REQ-COMPAT-001 制約 5(排他): 独立出力を完全停止させてから適用する。
	if (output_instance_ && output_instance_->GetState() != output_instance::State::Stopped) {
		output_instance_->Stop();
		int waited_ms = 0;
		while (output_instance_->GetState() != output_instance::State::Stopped &&
		       waited_ms < kStopWaitTimeoutMs) {
			Sleep(kStopWaitPollMs);
			waited_ms += kStopWaitPollMs;
		}
		if (output_instance_->GetState() != output_instance::State::Stopped) {
			obs_log(LOG_ERROR, "MonitorCompatController: timed out waiting for independent output to "
					   "stop; aborting compat mode enable");
			return false;
		}
	}

	const std::vector<compat_plan::SourceInfo> sources = EnumerateSourceInfos();
	const std::size_t mix_index = audio_track_map::ToMixIndex(audio_track_);

	// レビュー指摘(High-2b): Enable() はユーザー操作(有効化/デバイス変更)そのものなので、
	// キャッシュを強制的に再列挙する。
	const std::string default_device_id = GetCachedDefaultDeviceId(/*force_refresh=*/true);

	last_plan_ = compat_plan::BuildEnablePlan(sources, mix_index, output_device_id_, default_device_id);

	if (!last_plan_.warn_monitor_only.empty()) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: %zu MONITOR_ONLY source(s) left unchanged (REQ-COMPAT-001 制約1)",
			last_plan_.warn_monitor_only.size());
	}
	if (!last_plan_.self_silence_risk.empty()) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: %zu source(s) may self-silence (事実5, DO_NOT_SELF_MONITOR, "
			"output_device_id=\"%s\")",
			last_plan_.self_silence_risk.size(), output_device_id_.c_str());
	}

	// S9-1: obs_get_audio_monitoring_device の返却ポインタは libobs 内部バッファ。受領後すぐ
	// std::string へコピーする(次の obs_set_audio_monitoring_device で bfree されるため)。
	const char *cur_name = nullptr;
	const char *cur_id = nullptr;
	obs_get_audio_monitoring_device(&cur_name, &cur_id);
	const std::string original_device_name = cur_name ? cur_name : "";
	const std::string original_device_id = cur_id ? cur_id : "";

	// レビュー3回目残作業(Low-2): これから保存しようとしている「元」モニタリングデバイス id が、
	// これから適用する自分自身の出力先(output_device_id_、解決済み実 id)と一致する場合、それは
	// 真の「有効化前の元の値」ではない可能性が高い(compat-restore.json が give_up 等で削除された後、
	// OBS 側のモニタリングデバイスは前回セッションで適用した compat 出力先のまま残っている状態で
	// 再度 Enable() したケース等)。無効化時にこの id へ「復元」しても実害は無いが(既に自分の出力先
	// と同じため)、原因追跡のため 1 行 WARNING を出す。
	if (!original_device_id.empty() && original_device_id == output_device_id_) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: current OBS monitoring device (id=\"%s\") already matches this "
			"plugin's compat output device; saving it as the \"original\" device to restore to on "
			"disable (possibly a re-enable after a previous restore was given up)",
			output_device_id_.c_str());
	}

	// レビュー3回目指摘(High-1a): 既存ファイルがあれば復元進捗(restore_attempts/device_restored)を
	// リセットしてから使う(前セッションの EXIT が書き戻した値を引き継いで Disable() が復元を
	// スキップしてしまうのを防ぐ。WriteRestoreFileIfMissing() より前に行うことで、直後の
	// 「既存なら保持」判定に影響しない)。
	ResetRestoreProgress();

	// REQ-COMPAT-001 制約 2: 有効化時に保存(既存ファイルがあれば元値は上書きしない)。
	// レビュー3回目指摘(High-2): 保存に失敗したら monitoring_type を一切変更せず中断する(復元記録の
	// 無いまま変更すると Disable()/EXIT で元へ戻せなくなる)。
	if (!WriteRestoreFileIfMissing(original_device_name, original_device_id, last_plan_.to_change)) {
		obs_log(LOG_ERROR,
			"MonitorCompatController: failed to persist %s; aborting compat mode enable to "
			"avoid an unrecoverable monitoring type change",
			kRestoreFileName);
		return false;
	}

	// レビュー指摘(High-A): 残存 compat-restore.json がある状態(前回の Disable()/EXIT が復元を
	// 完了できず残した、または前回セッションのクラッシュ残骸)で再度 Enable() したとき、
	// WriteRestoreFileIfMissing() は「既に存在するので何もしない」だけで終わってしまい、今回新たに
	// to_change に入ったソースがファイルへ記録されない(Disable() 時に復元漏れになる)。
	// AppendSourcesToRestoreFile() は名前重複排除済みのため、ファイルを新規作成した直後に呼んでも
	// 副作用は無い(その場合は全件が重複として無視される)。
	// レビュー3回目指摘(High-2): 追記の永続化に失敗した場合、この Enable() では to_change の
	// monitoring_type を変更しない(復元記録の無いソースを変更すると復元不能になる)。次の Reapply()
	// の 2 秒 tick で managed_source_names_ に入っていない(=未適用の)ソースとして再試行される。
	std::vector<std::string> sources_to_apply;
	if (AppendSourcesToRestoreFile(last_plan_.to_change)) {
		sources_to_apply = last_plan_.to_change;
	} else {
		obs_log(LOG_WARNING,
			"MonitorCompatController: failed to persist %zu source(s) to %s; skipping their monitoring "
			"type change this time (will retry)",
			last_plan_.to_change.size(), kRestoreFileName);
	}

	obs_log(LOG_INFO,
		"MonitorCompatController: saved original monitoring device (name=\"%s\" id=\"%s\") and %zu source "
		"monitoring type(s) before applying compat mode",
		original_device_name.c_str(), original_device_id.c_str(), sources_to_apply.size());

	ApplySourceChanges(sources_to_apply);

	// レビュー指摘(Medium-E): この Enable() 呼び出しで対象とみなした(既に MONITOR_AND_OUTPUT だった
	// ものを含む)ソース名を「管理済み」として記録する。Reapply() の 2 秒タイマーはこの集合に無い
	// 名前だけを新規出現として追従する。
	managed_source_names_ = sources_to_apply;
	managed_source_names_.insert(managed_source_names_.end(), last_plan_.already_ok.begin(),
				     last_plan_.already_ok.end());

	const bool set_ok = obs_set_audio_monitoring_device(output_device_name_.c_str(), output_device_id_.c_str());
	if (!set_ok) {
		obs_log(LOG_ERROR,
			"MonitorCompatController: obs_set_audio_monitoring_device failed (name=\"%s\" id=\"%s\")",
			output_device_name_.c_str(), output_device_id_.c_str());
	}

	enabled_ = true;
	++apply_count_;
	obs_log(LOG_INFO,
		"MonitorCompatController: compat mode enabled (track=%d device_id=\"%s\" changed=%zu already_ok=%zu "
		"warned=%zu)",
		audio_track_, output_device_id_.c_str(), sources_to_apply.size(), last_plan_.already_ok.size(),
		last_plan_.warn_monitor_only.size());
	return true;
}

void MonitorCompatController::Reapply()
{
	if (!enabled_)
		return;

	const char *cur_name = nullptr;
	const char *cur_id = nullptr;
	obs_get_audio_monitoring_device(&cur_name, &cur_id);
	const std::string current_id = cur_id ? cur_id : "";

	bool changed = false;
	if (current_id != output_device_id_) {
		// S9-3/S9-6(制約 3): OBS 側の上書き(プロファイル切替の LoadProfile() 等)を検出→再設定。
		// レビュー指摘(High-2a): この 2 秒タイマー由来の呼び出しで obs_set_audio_monitoring_device
		// が失敗し続けると、以前は毎 2 秒無条件でリトライ+ログを出していた(音切れ+ログ洪水)。
		// 戻り値を見て、失敗時は指数バックオフ(2s→4s→8s→…上限30s)で次回試行時刻を延ばし、ログは
		// 状態変化(新規失敗/復帰)時のみ出す。
		const uint64_t now_ns = os_gettime_ns();
		if (now_ns >= device_apply_retry_at_ns_) {
			const bool set_ok =
				obs_set_audio_monitoring_device(output_device_name_.c_str(), output_device_id_.c_str());
			if (set_ok) {
				if (device_mismatch_last_logged_) {
					obs_log(LOG_INFO,
						"MonitorCompatController: reapplied monitoring device (was id=\"%s\", "
						"now id=\"%s\")",
						current_id.c_str(), output_device_id_.c_str());
				}
				device_apply_backoff_ns_ = 0;
				device_apply_retry_at_ns_ = 0;
				device_mismatch_last_logged_ = false;
				changed = true;
			} else {
				if (!device_mismatch_last_logged_) {
					obs_log(LOG_WARNING,
						"MonitorCompatController: obs_set_audio_monitoring_device failed while "
						"reapplying (id=\"%s\"); backing off (up to %llums between retries)",
						output_device_id_.c_str(),
						(unsigned long long)(kMaxDeviceApplyBackoffNs / 1000000ULL));
					device_mismatch_last_logged_ = true;
				}
				// windows.h の min/max マクロ(NOMINMAX 未定義)と衝突しないよう (std::min) と
				// 括弧で囲んで関数呼び出しであることを明示する。
				device_apply_backoff_ns_ =
					device_apply_backoff_ns_ == 0
						? kInitialDeviceApplyBackoffNs
						: (std::min)(device_apply_backoff_ns_ * 2, kMaxDeviceApplyBackoffNs);
				device_apply_retry_at_ns_ = now_ns + device_apply_backoff_ns_;
			}
		}
	} else if (device_mismatch_last_logged_ || device_apply_backoff_ns_ != 0) {
		// 一致が確認できた(OBS 側か直近の再試行で解消済み): バックオフ状態をリセットする。
		device_apply_backoff_ns_ = 0;
		device_apply_retry_at_ns_ = 0;
		device_mismatch_last_logged_ = false;
	}

	const std::vector<compat_plan::SourceInfo> sources = EnumerateSourceInfos();
	const std::size_t mix_index = audio_track_map::ToMixIndex(audio_track_);
	// レビュー指摘(High-2b): 2 秒タイマーから呼ばれるため毎回列挙し直さない(キャッシュ)。
	const std::string default_device_id = GetCachedDefaultDeviceId();

	compat_plan::EnablePlan plan =
		compat_plan::BuildEnablePlan(sources, mix_index, output_device_id_, default_device_id);

	// レビュー指摘(Medium-E): 2 秒タイマーの Reapply() が plan.to_change を無条件で
	// MONITOR_AND_OUTPUT へ強制すると、ユーザーが管理済みソースの monitoring type を手動で
	// (MONITOR_AND_OUTPUT 以外へ)変更した意図を毎回上書きしてしまう。managed_source_names_
	// (Enable() 時点+これまでの Reapply() で実際に変更/確認したソース名の集合)に含まれない、
	// 真に新規出現したソースだけを追従対象にする。ユーザーの変更を restore file から削除する
	// わけではない(無効化時の復元対象には引き続き残す)。
	std::vector<std::string> newly_seen;
	for (const auto &name : plan.to_change) {
		if (std::find(managed_source_names_.begin(), managed_source_names_.end(), name) ==
		    managed_source_names_.end())
			newly_seen.push_back(name);
	}

	if (!newly_seen.empty()) {
		// 新規に変更するソースは、まず compat-restore.json へ追記(永続化)してから
		// monitoring_type を変更する(レビュー3回目指摘 High-2: 逆順だと追記が失敗したときに
		// 復元記録の無いまま変更してしまい、Disable()/EXIT での復元から漏れる)。追記に失敗したら
		// この tick では変更をスキップし、次の 2 秒 tick で(managed_source_names_ に未追加のため)
		// 再試行する。
		if (AppendSourcesToRestoreFile(newly_seen)) {
			ApplySourceChanges(newly_seen);
			managed_source_names_.insert(managed_source_names_.end(), newly_seen.begin(), newly_seen.end());
			changed = true;
		}
		// レビュー最終指摘(Medium-A): 追記失敗のログは AppendSourcesToRestoreFile() 内で状態変化時
		// (初回失敗+回復)のみ出す(この 2 秒 tick ごとの呼び出しで二重に無条件ログを重ねない)。
		// 失敗時はここでは何もしない(次の 2 秒 tick で managed_source_names_ に未追加のため再試行)。
	}

	last_plan_ = plan;

	if (changed)
		++reapply_count_;
}

void MonitorCompatController::Disable(bool is_exit_context)
{
	RestoreFromFile(is_exit_context);
	enabled_ = false;
	last_plan_ = compat_plan::EnablePlan{};
	// レビュー指摘(Medium-E): 無効化したら管理済みソース集合もクリアする(次回 Enable() で作り直す)。
	managed_source_names_.clear();
}

void MonitorCompatController::RestoreStaleFileIfPresent()
{
	// FINISHED_LOADING で output_mode != compat のときに呼ぶ(クラッシュ耐性、S9-7)。enabled_ は
	// このプロセスでは false のはずだが、RestoreFromFile() 自体は enabled_ に依存しないため安全。
	// is_exit_context=false: 保存が有効な文脈(起動時)のため、全件確認/High-C の諦め判定に従って
	// ファイルを削除してよい。
	RestoreFromFile(/*is_exit_context=*/false);
}

void MonitorCompatController::WarnIfStaleRestoreFilePresent() const
{
	char *path = obs_module_config_path(kRestoreFileName);
	if (!path)
		return;

	if (os_file_exists(path)) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: %s is still present at obs_module_unload (EXIT was not received, "
			"or EXIT could not persist the restore, per S9-10); leaving it as-is, it will be restored "
			"at the next startup",
			kRestoreFileName);
	}

	bfree(path);
}

bool MonitorCompatController::WriteRestoreFileIfMissing(const std::string &monitoring_device_name,
							const std::string &monitoring_device_id,
							const std::vector<std::string> &touched_source_names) const
{
	char *path = obs_module_config_path(kRestoreFileName);
	if (!path) {
		obs_log(LOG_ERROR, "MonitorCompatController: obs_module_config_path failed; cannot write %s",
			kRestoreFileName);
		return false;
	}

	if (os_file_exists(path)) {
		// レビュー最終指摘(Medium-A): 存在するだけでなく実際に parse できるかを確認する。
		// 「既存なので保持」として素通りすると、破損ファイルの場合に以後
		// AppendSourcesToRestoreFile()/RestoreFromFile() が毎回 parse に失敗し、変更が一切
		// 進まないまま WARNING だけが Reapply() の 2 秒 tick ごとに出続ける不具合になる。
		obs_data_t *existing = obs_data_create_from_json_file_safe(path, "bak");
		if (existing) {
			obs_data_release(existing);
			// REQ-COMPAT-001 制約 2: 既存があれば上書きしない(再適用/クラッシュ後の再有効化で
			// 元値を失わないため)。
			obs_log(LOG_INFO, "MonitorCompatController: %s already exists; keeping original saved values",
				kRestoreFileName);
			bfree(path);
			return true;
		}

		// parse 不能: 固定名(連番なし)の .corrupt.bak へ改名退避してから、下の新規作成へ
		// フォールスルーする。同名の退避先が既にあれば os_rename() が上書きする
		// (MOVEFILE_REPLACE_EXISTING、platform-windows.c で確認済み)。退避自体に失敗したら
		// (読み取り専用ディレクトリ等)、復元記録の無いまま設定を変更してしまわないよう
		// Enable() を中断させる(false を返す)。
		char *corrupt_path = obs_module_config_path(kCorruptRestoreFileName);
		if (!corrupt_path) {
			obs_log(LOG_ERROR,
				"MonitorCompatController: obs_module_config_path failed; cannot move corrupt %s aside",
				kRestoreFileName);
			bfree(path);
			return false;
		}
		if (os_rename(path, corrupt_path) != 0) {
			obs_log(LOG_ERROR,
				"MonitorCompatController: %s exists but failed to parse, and could not be moved "
				"aside to %s; aborting compat mode enable to avoid an unrecoverable monitoring "
				"type change",
				kRestoreFileName, kCorruptRestoreFileName);
			bfree(corrupt_path);
			bfree(path);
			return false;
		}
		obs_log(LOG_WARNING,
			"MonitorCompatController: %s existed but failed to parse; moved it aside to %s and "
			"creating a fresh one",
			kRestoreFileName, kCorruptRestoreFileName);
		bfree(corrupt_path);
		// フォールスルーして新規作成する(path は既に空いている)。
	}

	char *dir = obs_module_config_path(nullptr);
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "monitoring_device_name", monitoring_device_name.c_str());
	obs_data_set_string(data, "monitoring_device_id", monitoring_device_id.c_str());

	obs_data_array_t *array = obs_data_array_create();
	for (const auto &name : touched_source_names) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name.c_str());
		obs_data_set_int(item, "monitoring_type", (long long)compat_plan::MonitoringType::None);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "sources", array);
	obs_data_array_release(array);

	const bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
	if (!ok)
		obs_log(LOG_ERROR, "MonitorCompatController: failed to save %s", kRestoreFileName);

	obs_data_release(data);
	bfree(path);
	return ok;
}

void MonitorCompatController::ResetRestoreProgress() const
{
	// レビュー3回目指摘(High-1a): AppendSourcesToRestoreFile() と同じ read-modify-write パターン。
	// ファイルが無ければ何もしない(新規作成される場合は attempts=0/device_restored=false が既定値の
	// ため、リセット不要)。
	char *path = obs_module_config_path(kRestoreFileName);
	if (!path)
		return;

	if (!os_file_exists(path)) {
		bfree(path);
		return;
	}

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (!data) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: %s exists but failed to parse while resetting "
			"restore progress; leaving it as-is",
			kRestoreFileName);
		bfree(path);
		return;
	}

	// レビュー最終指摘(Medium-C): restore_attempts はここで 0 に戻さない(kMaxRestoreAttempts に
	// よる「諦め」判定の消費カウンタが再有効化のたびに無効化されてしまうため)。device_restored のみ
	// リセットする(前セッションの EXIT が書き戻した true を引き継いで Disable() が復元をスキップ
	// してしまうのを防ぐという本来の目的はこれだけで達成できる)。
	obs_data_set_bool(data, "device_restored", false);
	if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
		obs_log(LOG_WARNING, "MonitorCompatController: failed to reset device_restored in %s on re-enable",
			kRestoreFileName);
	}

	obs_data_release(data);
	bfree(path);
}

bool MonitorCompatController::AppendSourcesToRestoreFile(const std::vector<std::string> &new_names) const
{
	if (new_names.empty())
		return true;

	char *path = obs_module_config_path(kRestoreFileName);
	if (!path)
		return false;

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (!data) {
		// Enable() が先に呼ばれていない(異常系)、またはファイルが破損している。追記のしようが
		// ないため諦める。レビュー最終指摘(Medium-A): この関数は Reapply() の 2 秒タイマーから
		// 毎 tick 呼ばれうるため、無条件ログだと同じ WARNING が延々と積み上がる。状態変化時
		// (初回失敗)のみ出す。
		if (!append_restore_failure_last_logged_) {
			obs_log(LOG_WARNING,
				"MonitorCompatController: %s missing or unparsable while trying to append new "
				"source(s); will keep retrying without repeating this warning",
				kRestoreFileName);
			append_restore_failure_last_logged_ = true;
		}
		bfree(path);
		return false;
	}

	obs_data_array_t *array = obs_data_get_array(data, "sources");
	if (!array)
		array = obs_data_array_create();

	// レビュー指摘(Low-2): 同名ソースを二重に追記しない(既存エントリの名前を先に集めておく)。
	std::vector<std::string> existing_names;
	const size_t existing_count = obs_data_array_count(array);
	existing_names.reserve(existing_count);
	for (size_t i = 0; i < existing_count; ++i) {
		obs_data_t *item = obs_data_array_item(array, i);
		if (item) {
			const char *name = obs_data_get_string(item, "name");
			if (name)
				existing_names.emplace_back(name);
			obs_data_release(item);
		}
	}

	bool appended_any = false;
	for (const auto &name : new_names) {
		if (std::find(existing_names.begin(), existing_names.end(), name) != existing_names.end())
			continue; // 既に記録済み(制約1: MONITOR_ONLY のソースはここに来ない前提だが、二重
				  // Reapply() 等で同名が複数回渡されるケースへの保険)。
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name.c_str());
		obs_data_set_int(item, "monitoring_type", (long long)compat_plan::MonitoringType::None);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
		existing_names.push_back(name);
		appended_any = true;
	}

	if (!appended_any) {
		obs_data_array_release(array);
		obs_data_release(data);
		bfree(path);
		// レビュー最終指摘(Medium-A): parse には成功した(=直前の失敗から回復した)ので、保留中の
		// 抑制フラグを解除する。
		if (append_restore_failure_last_logged_) {
			obs_log(LOG_INFO, "MonitorCompatController: %s is parseable again; resuming normal logging",
				kRestoreFileName);
			append_restore_failure_last_logged_ = false;
		}
		return true;
	}

	obs_data_set_array(data, "sources", array);
	obs_data_array_release(array);

	const bool ok = obs_data_save_json_safe(data, path, "tmp", "bak");
	if (!ok) {
		// レビュー最終指摘(Medium-A): 状態変化時(初回失敗)のみ WARNING を出す。
		if (!append_restore_failure_last_logged_) {
			obs_log(LOG_WARNING,
				"MonitorCompatController: failed to append %zu new source(s) to %s; will keep "
				"retrying without repeating this warning",
				new_names.size(), kRestoreFileName);
			append_restore_failure_last_logged_ = true;
		}
	} else if (append_restore_failure_last_logged_) {
		obs_log(LOG_INFO, "MonitorCompatController: successfully appended to %s after a previous failure",
			kRestoreFileName);
		append_restore_failure_last_logged_ = false;
	}

	obs_data_release(data);
	bfree(path);
	return ok;
}

void MonitorCompatController::RestoreFromFile(bool is_exit_context)
{
	char *path = obs_module_config_path(kRestoreFileName);
	if (!path)
		return;

	if (!os_file_exists(path)) {
		bfree(path); // 復元するものが無い(未有効化、または既に復元済み)。
		return;
	}

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (!data) {
		obs_log(LOG_WARNING, "MonitorCompatController: %s exists but failed to parse; leaving it as-is",
			kRestoreFileName);
		bfree(path);
		return;
	}

	const char *device_name = obs_data_get_string(data, "monitoring_device_name");
	const char *device_id = obs_data_get_string(data, "monitoring_device_id");
	// レビュー指摘(High-C): 既存ファイル(キー無し)は obs_data の未設定キー既定値(int=0/bool=false)に
	// より自動的に attempts=0/device_restored=false 扱いになる。
	long long restore_attempts = obs_data_get_int(data, "restore_attempts");
	const bool device_already_restored = obs_data_get_bool(data, "device_restored");

	std::vector<std::pair<std::string, compat_plan::MonitoringType>> saved;
	obs_data_array_t *array = obs_data_get_array(data, "sources");
	if (array) {
		const size_t count = obs_data_array_count(array);
		for (size_t i = 0; i < count; ++i) {
			obs_data_t *item = obs_data_array_item(array, i);
			if (item) {
				const char *name = obs_data_get_string(item, "name");
				long long type_raw = obs_data_get_int(item, "monitoring_type");
				// レビュー指摘(Low-1、AC-04 保護): 未知の monitoring_type(例: 1=MONITOR_ONLY)を
				// 無条件で cast しない。制約1によりこのファイルへ保存されるのは常に None
				// (有効化前の値)のはずだが、ファイル破損・手動編集への防御として 0/2 以外は
				// None へフォールバックする(MONITOR_ONLY への誤復元はミックスを無音化させ
				// AC-04/AC-26b の bit-identical を壊す)。
				if (type_raw != static_cast<long long>(compat_plan::MonitoringType::None) &&
				    type_raw != static_cast<long long>(compat_plan::MonitoringType::MonitorAndOutput)) {
					obs_log(LOG_WARNING,
						"MonitorCompatController: %s has an unexpected "
						"monitoring_type=%lld for source \"%s\"; treating as None (制約1/"
						"AC-04 保護)",
						kRestoreFileName, type_raw, name ? name : "");
					type_raw = static_cast<long long>(compat_plan::MonitoringType::None);
				}
				if (name && name[0] != '\0')
					saved.emplace_back(name, static_cast<compat_plan::MonitoringType>(type_raw));
				obs_data_release(item);
			}
		}
		obs_data_array_release(array);
	}

	// present_source_names: 保存済み名前ごとに obs_get_source_by_name() で実在確認する(制約 2:
	// 「消えたソースはスキップ」)。全ソース列挙は不要。
	std::vector<std::string> present_names;
	present_names.reserve(saved.size());
	for (const auto &entry : saved) {
		obs_source_t *source = obs_get_source_by_name(entry.first.c_str());
		if (source) {
			present_names.push_back(entry.first);
			obs_source_release(source);
		}
	}

	const std::vector<compat_plan::RestoreEntry> plan = compat_plan::BuildRestorePlan(saved, present_names);

	// レビュー指摘(Critical-1/Medium-3): 復元ループ。(a) レース(直前の存在確認後に削除)で
	// 見つからなかった件数と、(b) ユーザーが有効化後に手動でこのソースを MONITOR_AND_OUTPUT 以外へ
	// 変更していた(=制約3寄りの保護: ユーザーの意図的な変更を無条件で上書きしない、Disable() 版)
	// ため復元をスキップした件数を、それぞれ別に数える。
	size_t not_found_in_loop = 0;
	size_t manual_override_skipped = 0;
	size_t restored = 0;
	// レビュー指摘(High-C): kMaxRestoreAttempts 到達時に「諦めて削除する」際、未復元のソース名を
	// WARNING で列挙するために集める(missing 分は下で追加する)。
	std::vector<std::string> unrestored_names;
	for (const auto &entry : plan) {
		obs_source_t *source = obs_get_source_by_name(entry.name.c_str());
		if (!source) {
			++not_found_in_loop; // レース(直前の存在確認後に削除された)。
			unrestored_names.push_back(entry.name);
			continue;
		}
		const compat_plan::MonitoringType current =
			ToCompatMonitoringType(obs_source_get_monitoring_type(source));
		if (current == compat_plan::MonitoringType::MonitorAndOutput) {
			obs_source_set_monitoring_type(source, ToObsMonitoringType(entry.original_type));
			++restored;
		} else {
			// レビュー指摘(Medium-3): 有効化後にユーザーが手動でこのソースの monitoring type を
			// 変更していた(もう MONITOR_AND_OUTPUT ではない)。compat 側が付けた値を前提とした
			// 復元をユーザーの明示的な変更で無条件に上書きしない。
			++manual_override_skipped;
		}
		obs_source_release(source);
	}

	const size_t missing = saved.size() - plan.size();
	if (missing > 0) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: %zu source(s) from %s no longer exist; skipped restoring them",
			missing, kRestoreFileName);
		for (const auto &entry : saved) {
			if (std::find(present_names.begin(), present_names.end(), entry.first) == present_names.end())
				unrestored_names.push_back(entry.first);
		}
	}
	if (manual_override_skipped > 0) {
		obs_log(LOG_INFO,
			"MonitorCompatController: %zu source(s) were manually changed away from "
			"MONITOR_AND_OUTPUT while compat mode was active; leaving them as-is instead of "
			"overwriting the user's change",
			manual_override_skipped);
	}

	// レビュー指摘(Medium-2): 保存された元デバイスが空文字列(有効化前に一度もモニタリング
	// デバイスが設定されていなかった等)なら、既定デバイスへ復元する(obs_set_audio_monitoring_device
	// は空文字列で false を返す、S9-2)。
	std::string restore_device_name = device_name ? device_name : "";
	std::string restore_device_id = device_id ? device_id : "";
	if (restore_device_id.empty() || restore_device_name.empty()) {
		restore_device_id = "default";
		const char *fallback_name = obs_module_text("ExternalAudioOutput.Dock.DefaultDeviceOption");
		restore_device_name = fallback_name ? fallback_name : "Default Device";
	}
	// レビュー指摘(High-C): device_restored==true(過去の試行で既に成功済み)なら再度
	// obs_set_audio_monitoring_device() を呼ばない。呼ぶたびに毎起動ユーザーの現在のモニタリング先を
	// 上書きし続けてしまう(ファイルが missing/race で残存し続けるケース)ため、初回成功後は
	// ソース側の復元(実在確認できたものだけ)だけを繰り返す。
	bool device_restored = device_already_restored;
	if (!device_already_restored) {
		device_restored =
			obs_set_audio_monitoring_device(restore_device_name.c_str(), restore_device_id.c_str());
	}

	obs_log(LOG_INFO,
		"MonitorCompatController: restored %zu/%zu source monitoring type(s) (%zu manually overridden, "
		"%zu missing); monitoring device %s",
		restored, saved.size(), manual_override_skipped, missing,
		device_restored ? (device_already_restored ? "already restored (previous attempt)" : "restored")
				: "left unchanged (invalid)");

	// レビュー最終指摘(Medium-C/Low-4): is_exit_context のときは restore_attempts を増分しない
	// (EXIT でのランタイム復元は give_up 判定の「試行」として消費しない。次回起動時の
	// RestoreStaleFileIfPresent() が唯一の永続化される復元経路であり、EXIT 時の失敗は
	// kMaxRestoreAttempts に数えるべきではないため)。
	if (!is_exit_context)
		++restore_attempts;

	// レビュー指摘(Critical-1、High-B、High-C):
	//   - is_exit_context=true(S9-10): EXIT ハンドラでの変更はシーンコレクションに保存されないため、
	//     成否に関わらずファイルを絶対に削除しない(次回起動時の RestoreStaleFileIfPresent() が
	//     唯一の永続化される復元経路)。「諦め」判定もここでは行わない。
	//   - all_confirmed: 復元プランの全件が復元成功(または現存ソース全件成功かつ欠落 0)。
	//     manual_override_skipped はユーザーの意図的な変更であり「復元漏れ」ではないため、削除可否
	//     には影響させない。デバイス復元の成否(device_restored)も、OBS 側モニタリングデバイスは
	//     ランタイムのみ(S9-2、恒久破壊しない)ため削除可否には影響させない。
	//   - give_up: missing/race が kMaxRestoreAttempts 回の試行を経ても解消しない場合、ファイルを
	//     恒久残存させて毎起動 obs_set_audio_monitoring_device() でユーザーのモニタリング先を
	//     上書きし続けるより、諦めて削除する(未復元ソース名は WARNING で列挙する)。
	const bool all_confirmed = (missing == 0 && not_found_in_loop == 0);
	const bool give_up = !is_exit_context && !all_confirmed && restore_attempts >= kMaxRestoreAttempts;
	const bool delete_file = !is_exit_context && (all_confirmed || give_up);

	if (give_up) {
		std::string joined;
		for (size_t i = 0; i < unrestored_names.size(); ++i) {
			if (i > 0)
				joined += ", ";
			joined += unrestored_names[i];
		}
		obs_log(LOG_WARNING,
			"MonitorCompatController: giving up restoring %s after %lld attempt(s); still-unrestored "
			"source(s): %s",
			kRestoreFileName, restore_attempts, joined.c_str());
	}

	if (!delete_file) {
		// レビュー3回目指摘(Medium-1): 書き戻しの保存が一度失敗したら、このセッション中は以後の
		// 自動復元(RestoreStaleFileIfPresent()/Disable() 経由)で書き戻しを再試行しない。保存が
		// 失敗し続ける環境(読み取り専用ディレクトリ等)で毎回 obs_set_audio_monitoring_device() を
		// 呼び直し、restore_attempts が進まないまま無限リトライになるのを防ぐ。WARNING は初回のみ。
		if (!restore_persist_failed_) {
			// レビュー3回目指摘(High-1b): is_exit_context のときは device_restored を true として
			// 書き戻さない。EXIT でのランタイム復元(見た目上の一貫性)は今セッション限りであり、
			// 次回起動時に OBS 自身が basic.ini から monitoring device を再適用する(S9-3)ため、
			// このプラグインの復元が「次回も有効」であるかのように記録しない。
			// レビュー最終指摘(Medium-C/Low-4): is_exit_context のときは restore_attempts も
			// 書き戻さない(そもそも上で増分していないため、data 内の値は読み込み時のまま。EXIT の
			// 復元は give_up 判定の試行回数に数えない)。
			if (!is_exit_context)
				obs_data_set_int(data, "restore_attempts", restore_attempts);
			obs_data_set_bool(data, "device_restored", is_exit_context ? false : device_restored);
			if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
				restore_persist_failed_ = true;
				obs_log(LOG_WARNING,
					"MonitorCompatController: failed to persist restore_attempts/device_restored to "
					"%s; will not retry persisting automatically this session (avoids repeated "
					"monitoring device overwrites)",
					kRestoreFileName);
			}
		}
	}

	obs_data_release(data);

	++restore_count_;

	if (delete_file) {
		DeleteRestoreFile();
	} else if (is_exit_context) {
		obs_log(LOG_WARNING,
			"MonitorCompatController: keeping %s at EXIT (S9-10: monitoring_type changes made here are "
			"not persisted to the scene collection); will restore again from it on next startup",
			kRestoreFileName);
	} else {
		obs_log(LOG_WARNING,
			"MonitorCompatController: keeping %s because %zu source(s) could not be confirmed restored "
			"(missing=%zu race=%zu, attempt %lld/%d); will retry restoring from it on next startup/disable",
			kRestoreFileName, missing + not_found_in_loop, missing, not_found_in_loop, restore_attempts,
			kMaxRestoreAttempts);
	}

	bfree(path);
}

bool MonitorCompatController::DeleteRestoreFile() const
{
	char *path = obs_module_config_path(kRestoreFileName);
	if (!path)
		return false;

	bool ok = true;
	if (os_file_exists(path)) {
		ok = (os_unlink(path) == 0);
		if (ok) {
			obs_log(LOG_INFO, "MonitorCompatController: deleted %s after successful restore",
				kRestoreFileName);
		} else {
			obs_log(LOG_WARNING, "MonitorCompatController: failed to delete %s after restore",
				kRestoreFileName);
		}
	}
	bfree(path);
	return ok;
}
