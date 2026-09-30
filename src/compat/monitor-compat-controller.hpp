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
//
// 互換モード(output_mode="obs_monitor_compat")の実体。**UI スレッド(frontend イベント含む)でのみ
// 動作する**(qt-ui-and-lifecycle.md、slice-9.md 実装変更範囲)。RT スレッド(libobs audio callback /
// WASAPI render thread)には一切触れない。libobs 公開 API(obs_enum_sources/
// obs_source_set_monitoring_type/obs_set_audio_monitoring_device/obs_data_*)のみを使う。
//
// 排他制御(REQ-COMPAT-001 制約 5): Enable() は独立出力(OutputInstance)が Stopped でなければ
// Stop() を要求し、有限タイムアウトまで GetState()==Stopped を待ってから互換モードを適用する
// (「現モード完全停止 → 新モード適用」を直列に行う)。呼び出し元(ExternalOutputDock)は Qt UI
// スレッドをこの待ち時間だけブロックすることを許容する(OutputInstance::Shutdown() の EXIT 待ちと
// 同種の設計判断)。

#pragma once

#include "compat-plan.hpp"

#include <cstdint>
#include <string>
#include <vector>

class OutputInstance;

class MonitorCompatController {
public:
	explicit MonitorCompatController(OutputInstance *output_instance);

	MonitorCompatController(const MonitorCompatController &) = delete;
	MonitorCompatController &operator=(const MonitorCompatController &) = delete;

	// 有効化(Dock の OutputMode 切替 / 開始・停止ボタン / FINISHED_LOADING 自動適用から呼ぶ)。
	// audio_track: 対象トラック(1〜6)。output_device_id/output_device_name: config.json の
	// device_id/device_name(独立出力と共通の「出力先」選択、REQUIREMENTS.md §5)。
	// 既に有効な場合は対象トラック/出力先を更新して Reapply() 相当を行う(冪等)。
	// 戻り値: 適用できたか(false は独立出力の停止待ちタイムアウト等)。
	bool Enable(int audio_track, const std::string &output_device_id, const std::string &output_device_name);

	// 無効化(Dock の OutputMode 切替 / 開始・停止ボタン / EXIT から呼ぶ)。compat-restore.json から
	// 元のモニタリングデバイス・各ソースの monitoring type を復元し、全件確認できればファイルを削除する。
	// 有効化していなくても安全(ファイルが残っていれば復元だけ行う、EXIT の保険)。
	// is_exit_context=true(OnExit() 専用、レビュー指摘 High-B): S9-10 により EXIT ハンドラ内での
	// obs_source_set_monitoring_type 変更はシーンコレクションに保存されない(OBS は EXIT より前に
	// SaveProjectNow() 済み)。ランタイム復元(見た目上の一貫性)は行うが、次回起動時の
	// RestoreStaleFileIfPresent() が唯一の永続化される復元経路であるため、この文脈では成否に関わらず
	// ファイルを削除しない。
	void Disable(bool is_exit_context = false);

	// 再適用(PROFILE_CHANGED / Dock の 2 秒タイマーから呼ぶ、REQ-COMPAT-001 制約 3)。現在の
	// モニタリングデバイスが期待値と違えば再設定し、新規追加ソースを MONITOR_AND_OUTPUT へ追従させる。
	// 有効化していなければ no-op。
	void Reapply();

	// 起動時のクラッシュ耐性復元(FINISHED_LOADING、output_mode!=compat のときに呼ぶ)。
	// compat-restore.json が残っていればファイルから直接復元する(in-memory 状態に依存しない)。
	// ファイルが無ければ no-op。
	void RestoreStaleFileIfPresent();

	// レビュー指摘(High-B/Low-G): obs_module_unload() の保険専用。EXIT を経ずに落ちた場合でも、
	// S9-10 によりファイル復元は次回起動時にしか永続化されないため、ここではファイル復元も
	// obs_set_audio_monitoring_device() 呼び出しも一切行わない。compat-restore.json が残っていれば
	// 「次回起動時に復元される」旨を LOG_WARNING で示すだけ(終了直前に全 monitor を再生成する
	// Low-G のリスクを避ける)。
	void WarnIfStaleRestoreFilePresent() const;

	bool IsEnabled() const { return enabled_; }

	// 現在の適用対象(Dock が audio_track/デバイス変更を再適用する際に使う。IsEnabled()==false の
	// ときは直近 Enable() 呼び出し時点の値、または既定値)。
	int AudioTrack() const { return audio_track_; }
	const std::string &OutputDeviceId() const { return output_device_id_; }
	const std::string &OutputDeviceName() const { return output_device_name_; }

	// Dock の警告表示用。直近の Enable()/Reapply() で得た分類結果。
	const compat_plan::EnablePlan &LastPlan() const { return last_plan_; }

	// 診断ダンプ用のイベントカウンタ(slice-9.md 実装変更範囲: diag ダンプへの追加)。
	uint64_t ApplyCount() const { return apply_count_; }
	uint64_t RestoreCount() const { return restore_count_; }
	uint64_t ReapplyCount() const { return reapply_count_; }

private:
	std::vector<compat_plan::SourceInfo> EnumerateSourceInfos() const;
	void ApplySourceChanges(const std::vector<std::string> &to_change) const;
	// device_name が空、または device_id=="default" で name 未指定のときにデバイス列挙から解決する
	// (obs_set_audio_monitoring_device は name/id とも非空文字列必須、S9-2)。
	std::string ResolveDeviceDisplayName(const std::string &device_id, const std::string &device_name) const;

	// レビュー指摘(High-2b): 自己無音警告(事実5)判定用の既定デバイス id をキャッシュする。Reapply()
	// は 2 秒タイマーから呼ばれるため、毎回 AudioDeviceManager::Enumerate()(COM 列挙)を呼ばない。
	// output_instance_->DeviceTopologyChangeCount()(DefaultDeviceChanged/DeviceArrived を受信する
	// たびに増分)が前回と変わっていれば再列挙する。force_refresh=true(Enable() = ユーザー操作)は
	// 無条件で再列挙する。
	std::string GetCachedDefaultDeviceId(bool force_refresh = false);

	// レビュー3回目指摘(High-1a): 既存 compat-restore.json がある状態で Enable() するとき、
	// device_restored を false へ読み戻す(AppendSourcesToRestoreFile() と同じ read-modify-write
	// パターン)。「compat のまま OBS 終了→再起動→ユーザーが無効化」のケースで、前回セッションの
	// EXIT が書き戻した device_restored=true を引き継いで Disable() が
	// obs_set_audio_monitoring_device() の呼び出しをスキップしてしまう(=デバイスが復元されない)不具合
	// を防ぐ。ファイルが無ければ no-op。
	// レビュー最終指摘(Medium-C): restore_attempts はここでは触らない(以前は 0 へ戻していたが、
	// これは kMaxRestoreAttempts による「諦め」判定の消費カウンタであり、再有効化のたびに
	// リセットすると上限が実質無効化されてしまうため)。
	void ResetRestoreProgress() const;
	// 戻り値: 元設定(モニタリングデバイス名/id・対象ソース)の保存に成功したか。false のときは
	// 呼び出し元(Enable())が monitoring_type の変更・obs_set_audio_monitoring_device() を行わず
	// 中断する(レビュー3回目指摘 High-2: 復元記録の無いまま設定を変更すると復元不能になるため)。
	bool WriteRestoreFileIfMissing(const std::string &monitoring_device_name,
				       const std::string &monitoring_device_id,
				       const std::vector<std::string> &touched_source_names) const;
	// 戻り値: new_names の追記(または「追記不要」)が確実に永続化されたか。false のときは
	// 呼び出し元(Enable()/Reapply())が new_names に対する obs_source_set_monitoring_type() 呼び出しを
	// 行わず、managed_source_names_ にも加えない(レビュー3回目指摘 High-2: 復元記録の無いソースを
	// 変更すると Disable()/EXIT での復元から漏れるため。次 tick の Reapply() で再試行される)。
	bool AppendSourcesToRestoreFile(const std::vector<std::string> &new_names) const;
	// ファイルを読み、復元プランを適用する。ファイルが無ければ no-op。restore_count_ を増分する
	// (呼び出し元が Enable()/Disable() どちらの文脈かは問わない)。
	// is_exit_context=true: レビュー指摘 High-B。ファイル削除・「諦め」判定(High-C の
	// restore_attempts>=3)のいずれも行わず、無条件でファイルを残す(S9-10、次回起動時の唯一の
	// 永続化される復元経路を保つ)。restore_attempts/device_restored の書き戻しは通常どおり行う。
	// レビュー3回目指摘(High-1b): is_exit_context=true のときは device_restored を true として
	// 書き戻さない(EXIT でのランタイム復元は今セッション限りで、次回起動時のデバイス状態を保証しない
	// ため)。
	void RestoreFromFile(bool is_exit_context = false);
	bool DeleteRestoreFile() const;

	OutputInstance *output_instance_ = nullptr; // 非所有(生存はプラグイン全体と同じ)

	bool enabled_ = false;
	int audio_track_ = 6;
	std::string output_device_id_;
	std::string output_device_name_;
	compat_plan::EnablePlan last_plan_;

	// レビュー指摘(Medium-E): Enable()/Reapply() が実際に MONITOR_AND_OUTPUT へ変更した(=
	// compat-restore.json に記録した)ソース名の集合。Reapply() の 2 秒タイマーはこの集合に含まれない
	// 「真に新規出現したソース」だけを追従対象にする。ユーザーが管理済みソースの monitoring type を
	// 手動で変更した場合はその意図を尊重し、再強制しない。Enable() の新規有効化(!enabled_)で
	// last_plan_ の to_change/already_ok から作り直す。Disable() でクリアする。
	std::vector<std::string> managed_source_names_;

	uint64_t apply_count_ = 0;
	uint64_t restore_count_ = 0;
	uint64_t reapply_count_ = 0;

	// レビュー指摘(High-2b): GetCachedDefaultDeviceId() のキャッシュ状態(UI スレッドの単一箇所
	// からのみ読み書きされるため atomic 不要)。
	std::string default_device_id_cache_;
	uint64_t default_device_id_cache_topology_count_ = 0;
	bool default_device_id_cache_valid_ = false;
	// レビュー指摘(Medium-F): トポロジカウンタが変化しないセッション(DeviceNotifier 登録失敗等)でも
	// キャッシュが無期限に古くならないよう、時間ベースの最低更新間隔を併用する(GetCachedDefaultDeviceId
	// 内の kDefaultDeviceCacheMaxAgeNs)。
	uint64_t default_device_id_cache_updated_at_ns_ = 0;

	// レビュー指摘(High-2a): Reapply() での obs_set_audio_monitoring_device 失敗時の指数バックオフ
	// (2s→4s→8s→…上限30s)状態。Reapply() のみが読み書きする。
	uint64_t device_apply_backoff_ns_ = 0;
	uint64_t device_apply_retry_at_ns_ = 0;
	bool device_mismatch_last_logged_ = false;

	// レビュー3回目指摘(Medium-1): RestoreFromFile() の restore_attempts/device_restored 書き戻しが
	// 保存失敗した場合、このセッションでの以後の自動復元(RestoreStaleFileIfPresent()/Disable() 経由)
	// では書き戻しを再試行しない(無限リトライ→毎起動 obs_set_audio_monitoring_device() 上書きを防ぐ)。
	// WARNING は初回の失敗時に 1 回だけ出す。
	bool restore_persist_failed_ = false;

	// レビュー最終指摘(Medium-A): AppendSourcesToRestoreFile() が parse 不能/保存失敗で false を
	// 返したときの「直近ログ済みか」フラグ。Reapply() の 2 秒タイマーから毎 tick 呼ばれるため、
	// 無条件ログだと同じ WARNING が延々と積み上がる。状態変化時(初回失敗+回復)のみログを出す。
	// mutable: AppendSourcesToRestoreFile() が const メソッドのため。
	mutable bool append_restore_failure_last_logged_ = false;
};
