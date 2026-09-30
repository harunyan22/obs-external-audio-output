// 対応 REQ: REQ-AUDIO-005, REQ-NFR-002 / Slice 3
//
// src/audio/drift-compensator.hpp(水位ベースのドリフト補正、D-02 MVP)の単体テスト。
// libobs に依存しないため、プラグイン本体とは別にリンクして ctest から実行する。
// OnWake() は本来 WASAPI render thread から毎 wake 呼ばれる想定だが、このテストはシングル
// スレッドから繰り返し呼び出すことで EMA 収束・ヒステリシス・レート上限・モード解除の
// 振る舞いを検証する。
//
// レビュー指摘 H-1/H-2/Medium-1 に伴い、DriftCompensator のコンストラクタ/OnWake() の
// シグネチャが変わった(チャンク単位 → frames 実効水位、wake_interval_ms → device_sample_rate_hz、
// 統計計上は NotifyApplied() 経由)。多くのテストで「device_sample_rate_hz を極端に小さくして
// tau_frames をほぼ 0 にし、frames_this_wake>=1 の呼び出しごとに EMA が現在値へほぼ即座に
// 追従する(alpha≈1)」トリックを使い、モード遷移の境界を frames 整数値で決定的に検証する。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../../src/audio/drift-compensator.hpp"

TEST_CASE("EMA: frames 進行量ベースの alpha で一定水位に収束する(現実的な wake サイズ)")
{
	// target_level_frames=2048(2 チャンク相当)、frames_per_chunk=1024、capacity_frames=4096、
	// device_sample_rate_hz=48000(現実的な値)。frames_this_wake=1024 を毎回渡す
	// (実運用で frames_this_wake==0 の wake は WasapiOutput::RenderLoop が available==0 で
	// スキップするため呼ばれない)。
	DriftCompensator comp(2048.0, 1024.0, 4096.0, 48000.0);

	for (int i = 0; i < 2000; ++i)
		comp.OnWake(8192.0, 1024);

	CHECK(comp.EmaLevelFrames() > 8000.0);
	CHECK(comp.EmaLevelFrames() <= 8192.0);
}

TEST_CASE("EMA: 初期値は target_level_frames")
{
	DriftCompensator comp(3000.0, 1000.0, 8000.0, 48000.0);
	CHECK(comp.EmaLevelFrames() == doctest::Approx(3000.0));
}

TEST_CASE("device_sample_rate_hz <= 0 はフォールバック(tau=0 → alpha=1.0、EMA が即座に現在値へ追従)")
{
	DriftCompensator comp(2000.0, 1000.0, 8000.0, 0.0);
	comp.OnWake(9000.0, 1);
	CHECK(comp.EmaLevelFrames() == doctest::Approx(9000.0));
}

TEST_CASE("frames_this_wake==0 の wake は EMA を更新しない")
{
	DriftCompensator comp(2000.0, 1000.0, 8000.0, 48000.0);
	CHECK(comp.OnWake(9000.0, 0) == DriftCompensator::Decision::None);
	CHECK(comp.EmaLevelFrames() == doctest::Approx(2000.0)); // 初期値のまま。
}

TEST_CASE("ヒステリシス: 水位が target+発動閾値 を超えると Drop モードに入り、target+解除閾値 以下で解除される")
{
	// device_sample_rate_hz を極端に小さくして alpha≈1(EMA が毎回ほぼ即座に現在値へ追従する)にし、
	// モード遷移の境界を frames 整数値で決定的にテストする。target=2000、frames_per_chunk=1000 →
	// 発動閾値=max(250,128)=250、解除閾値=1000/8=125。
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	// target+発動閾値 = 2250 ちょうどでは間引きモードに入らない(境界は「超える」)。
	comp.OnWake(2250.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);

	// target+発動閾値 を超える(2251 > 2250)と Drop モードに入る。
	comp.OnWake(2251.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Drop);

	// target+解除閾値 = 2125 を上回っている間(2126)は解除されない。
	comp.OnWake(2126.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Drop);

	// target+解除閾値 = 2125 以下(2125)に戻ると解除される。
	comp.OnWake(2125.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);
}

TEST_CASE("ヒステリシス: 水位が target-発動閾値 を下回ると Insert モードに入り、target-解除閾値 以上で解除される")
{
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	// target-発動閾値 = 1750 ちょうどでは挿入モードに入らない。
	comp.OnWake(1750.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);

	// target-発動閾値 を下回る(1749 < 1750)と Insert モードに入る。
	comp.OnWake(1749.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Insert);

	// target-解除閾値 = 1875 を下回っている間(1874)は解除されない。
	comp.OnWake(1874.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Insert);

	// target-解除閾値 = 1875 以上(1875)に戻ると解除される。
	comp.OnWake(1875.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);
}

TEST_CASE("レート上限: 最大 1 回の補正 / 500 frames を超えない(Drop、統計は NotifyApplied 経由でのみ加算)")
{
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	comp.OnWake(2300.0, 1); // Drop モードに入る(frames_since_last_correction_ は 1)。
	REQUIRE(comp.CurrentMode() == DriftCompensator::Mode::Drop);

	// 累計 500 frames に達するまでは補正なし(1 + 100*4 = 401 < 500)。
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.DroppedFrames() == 0);

	// 累計 500 frames(401+99)に達すると Drop が「決定」されるが、まだ NotifyApplied を呼んでいない
	// ため統計は加算されない(レビュー指摘 H-2)。
	CHECK(comp.OnWake(2300.0, 99) == DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 0);

	// RenderLoop が実際に適用できた時点で NotifyApplied を呼んで初めて統計が加算される。
	comp.NotifyApplied(DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 1);

	// リセット直後はまた 500 frames 溜まるまで補正なし。
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.DroppedFrames() == 1);

	CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::Drop);
	comp.NotifyApplied(DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 2);
}

TEST_CASE("レート上限: 最大 1 回の補正 / 500 frames を超えない(Insert、統計は NotifyApplied 経由でのみ加算)")
{
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	comp.OnWake(1700.0, 1);
	REQUIRE(comp.CurrentMode() == DriftCompensator::Mode::Insert);

	CHECK(comp.OnWake(1700.0, 250) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(1700.0, 248) == DriftCompensator::Decision::None); // 累計 1+250+248=499 < 500
	CHECK(comp.InsertedFrames() == 0);

	// 累計 500 ちょうどで Insert が決定されるが、NotifyApplied を呼ぶまでは統計に出ない。
	CHECK(comp.OnWake(1700.0, 1) == DriftCompensator::Decision::Insert);
	CHECK(comp.InsertedFrames() == 0);

	comp.NotifyApplied(DriftCompensator::Decision::Insert);
	CHECK(comp.InsertedFrames() == 1);
}

TEST_CASE("H-2: 決定を返しても NotifyApplied を呼ばなければ次 wake で同じ決定が返り続ける(未適用の再挑戦)")
{
	// RenderLoop がこの wake で補正を適用できなかったケース(挿入用の last_frame_ がまだ無い、
	// または間引き用のチャンク境界がこの wake 内に来なかった等)を模擬する。
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	comp.OnWake(2300.0, 1); // Drop モードへ。
	for (int i = 0; i < 4; ++i)
		CHECK(comp.OnWake(2300.0, 100) == DriftCompensator::Decision::None);
	CHECK(comp.OnWake(2300.0, 99) == DriftCompensator::Decision::Drop); // 累計 500 frames で決定。
	CHECK(comp.DroppedFrames() == 0);

	// NotifyApplied を呼ばずに次 wake を進めても、同じ Drop が返り続ける(自動的な再挑戦)。
	CHECK(comp.OnWake(2300.0, 1) == DriftCompensator::Decision::Drop);
	CHECK(comp.OnWake(2300.0, 1) == DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 0); // 未適用の間は一切計上されない。

	// ようやく適用できた wake で NotifyApplied を呼ぶと、そこで初めて計上・カウンタリセットされる。
	comp.NotifyApplied(DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 1);
	CHECK(comp.OnWake(2300.0, 1) == DriftCompensator::Decision::None); // リセット直後は補正なし。
}

TEST_CASE("モード解除: Normal に戻ると frames_since_last_correction が溜まっていても補正しない")
{
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	comp.OnWake(2300.0, 1); // Drop モードに入る
	CHECK(comp.OnWake(2300.0, 1000) == DriftCompensator::Decision::Drop);
	comp.NotifyApplied(DriftCompensator::Decision::Drop);
	CHECK(comp.DroppedFrames() == 1);

	// target に戻して Normal へ解除する。
	comp.OnWake(2000.0, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);

	// Normal のままなら、frames_this_wake をいくら積んでも補正されない。
	CHECK(comp.OnWake(2000.0, 10000) == DriftCompensator::Decision::None);
	CHECK(comp.DroppedFrames() == 1);
	CHECK(comp.InsertedFrames() == 0);
}

TEST_CASE("実運用条件: 毎 wake frames_this_wake>0 でも EMA が閾値を跨いでモード遷移する")
{
	// device_sample_rate_hz=48000(現実的な値、alpha≈1 のトリックを使わない)。
	// target_level_frames=2048、frames_per_chunk=1024 → 発動閾値=256、解除閾値=128、
	// tau_frames=2.0*48000=96000。
	DriftCompensator comp(2048.0, 1024.0, 4096.0, 48000.0);
	REQUIRE(comp.CurrentMode() == DriftCompensator::Mode::Normal);

	// 高い水位(6000 frames)を現実的な wake サイズ(1024 frames/wake)で供給し続けると、
	// 数 wake で EMA が発動閾値を超えて Drop モードに入る(理論値: 約 7 wake で到達)。
	for (int i = 0; i < 200 && comp.CurrentMode() == DriftCompensator::Mode::Normal; ++i)
		comp.OnWake(6000.0, 1024);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Drop);

	// 目標水位に戻したデータを供給し続けると、EMA が減衰して解除閾値内へ戻り Normal へ解除される。
	for (int i = 0; i < 2000 && comp.CurrentMode() == DriftCompensator::Mode::Drop; ++i)
		comp.OnWake(2048.0, 1024);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Normal);
}

TEST_CASE("frames 実効水位: チャンク境界に依存しない frames 値でも閾値判定が機能する"
	  "(RenderLoop の Level()*frames_per_chunk+carry 相当)")
{
	// target=2000、frames_per_chunk=1000 → 発動閾値=250。
	// 例: RingBuffer::Level()==3(3 チャンク分)+ carry 残り 210 frames = 3210 frames
	// (WasapiOutput::RenderLoop が計算する「frames 実効水位」と同じ式。レビュー指摘 H-1)。
	// チャンク整数(3)のままでは target(2000)との差が粗くしか分からないが、frames 換算(3210)なら
	// diff=1210 > 250 で明確に Drop 判定できる。
	DriftCompensator comp(2000.0, 1000.0, 100000.0, 1.0e-9);

	const double chunk_level = 3.0;
	const double frames_per_chunk = 1000.0;
	const double carry_remaining = 210.0;
	const double effective_level_frames = chunk_level * frames_per_chunk + carry_remaining;

	comp.OnWake(effective_level_frames, 1);
	CHECK(comp.CurrentMode() == DriftCompensator::Mode::Drop);
	CHECK(comp.EmaLevelFrames() == doctest::Approx(3210.0));
}

TEST_CASE("H-1: 発動閾値は「容量-目標水位」を超えないようクランプされる(構築時検証)")
{
	// capacity_frames が target_level_frames にごく近い(容量にほぼ余裕がない)極端な構成。
	// frames_per_chunk/4 の素朴な計算(=500)では発動閾値が容量を超えてしまうため、構築時に
	// クランプする(レビュー指摘 H-1: 警告ログ+補正無効化ではなく閾値そのものをクランプする)。
	DriftCompensator comp(1000.0, 2000.0, 1010.0, 48000.0); // capacity-target = 10、素の閾値候補は 500。

	CHECK(comp.TriggerThresholdFrames() > 0.0);
	CHECK(comp.TriggerThresholdFrames() < 10.0);
	CHECK(comp.ReleaseThresholdFrames() >= 0.0);
	CHECK(comp.ReleaseThresholdFrames() < comp.TriggerThresholdFrames());
}

TEST_CASE("H-1: 目標水位が容量以上という異常構成でも発動閾値は正の最小値にフォールバックする")
{
	DriftCompensator comp(5000.0, 1000.0, 4000.0, 48000.0); // capacity(4000) < target(5000)。
	CHECK(comp.TriggerThresholdFrames() == doctest::Approx(1.0));
}
