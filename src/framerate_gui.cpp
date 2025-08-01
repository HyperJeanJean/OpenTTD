/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file framerate_gui.cpp GUI for displaying framerate/game speed information. */

#include "stdafx.h"

#include "framerate_type.h"
#include <chrono>
#include "gfx_func.h"
#include "newgrf_sound.h"
#include "window_gui.h"
#include "window_func.h"
#include "string_func.h"
#include "strings_func.h"
#include "console_func.h"
#include "console_type.h"
#include "company_base.h"
#include "ai/ai_info.hpp"
#include "ai/ai_instance.hpp"
#include "game/game.hpp"
#include "game/game_instance.hpp"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "zoom_func.h"

#include "widgets/framerate_widget.h"

#include <atomic>
#include <mutex>

#include "table/strings.h"

#include "safeguards.h"

static std::mutex _sound_perf_lock;
static std::atomic<bool> _sound_perf_pending;
static std::vector<TimingMeasurement> _sound_perf_measurements;

/**
 * Private declarations for performance measurement implementation
 */
namespace {

	/** Number of data points to keep in buffer for each performance measurement */
	const int NUM_FRAMERATE_POINTS = 512;
	/** %Units a second is divided into in performance measurements */
	const TimingMeasurement TIMESTAMP_PRECISION = 1000000;

	struct PerformanceData {
		/** Duration value indicating the value is not valid should be considered a gap in measurements */
		static const TimingMeasurement INVALID_DURATION = UINT64_MAX;

		/** Time spent processing each cycle of the performance element, circular buffer */
		std::array<TimingMeasurement, NUM_FRAMERATE_POINTS> durations{};
		/** Start time of each cycle of the performance element, circular buffer */
		std::array<TimingMeasurement, NUM_FRAMERATE_POINTS> timestamps{};
		/** Expected number of cycles per second when the system is running without slowdowns */
		double expected_rate = 0;
		/** Next index to write to in \c durations and \c timestamps */
		int next_index = 0;
		/** Last index written to in \c durations and \c timestamps */
		int prev_index = 0;
		/** Number of data points recorded, clamped to \c NUM_FRAMERATE_POINTS */
		int num_valid = 0;

		/** Current accumulated duration */
		TimingMeasurement acc_duration{};
		/** Start time for current accumulation cycle */
		TimingMeasurement acc_timestamp{};

		/**
		 * Initialize a data element with an expected collection rate
		 * @param expected_rate
		 * Expected number of cycles per second of the performance element. Use 1 if unknown or not relevant.
		 * The rate is used for highlighting slow-running elements in the GUI.
		 */
		explicit PerformanceData(double expected_rate) : expected_rate(expected_rate) { }

		/** Collect a complete measurement, given start and ending times for a processing block */
		void Add(TimingMeasurement start_time, TimingMeasurement end_time)
		{
			this->durations[this->next_index] = end_time - start_time;
			this->timestamps[this->next_index] = start_time;
			this->prev_index = this->next_index;
			this->next_index += 1;
			if (this->next_index >= NUM_FRAMERATE_POINTS) this->next_index = 0;
			this->num_valid = std::min(NUM_FRAMERATE_POINTS, this->num_valid + 1);
		}

		/** Begin an accumulation of multiple measurements into a single value, from a given start time */
		void BeginAccumulate(TimingMeasurement start_time)
		{
			this->timestamps[this->next_index] = this->acc_timestamp;
			this->durations[this->next_index] = this->acc_duration;
			this->prev_index = this->next_index;
			this->next_index += 1;
			if (this->next_index >= NUM_FRAMERATE_POINTS) this->next_index = 0;
			this->num_valid = std::min(NUM_FRAMERATE_POINTS, this->num_valid + 1);

			this->acc_duration = 0;
			this->acc_timestamp = start_time;
		}

		/** Accumulate a period onto the current measurement */
		void AddAccumulate(TimingMeasurement duration)
		{
			this->acc_duration += duration;
		}

		/** Indicate a pause/expected discontinuity in processing the element */
		void AddPause(TimingMeasurement start_time)
		{
			if (this->durations[this->prev_index] != INVALID_DURATION) {
				this->timestamps[this->next_index] = start_time;
				this->durations[this->next_index] = INVALID_DURATION;
				this->prev_index = this->next_index;
				this->next_index += 1;
				if (this->next_index >= NUM_FRAMERATE_POINTS) this->next_index = 0;
				this->num_valid += 1;
			}
		}

		/** Get average cycle processing time over a number of data points */
		double GetAverageDurationMilliseconds(int count)
		{
			count = std::min(count, this->num_valid);

			int first_point = this->prev_index - count;
			if (first_point < 0) first_point += NUM_FRAMERATE_POINTS;

			/* Sum durations, skipping invalid points */
			double sumtime = 0;
			for (int i = first_point; i < first_point + count; i++) {
				auto d = this->durations[i % NUM_FRAMERATE_POINTS];
				if (d != INVALID_DURATION) {
					sumtime += d;
				} else {
					/* Don't count the invalid durations */
					count--;
				}
			}

			if (count == 0) return 0; // avoid div by zero
			return sumtime * 1000 / count / TIMESTAMP_PRECISION;
		}

		/** Get current rate of a performance element, based on approximately the past one second of data */
		double GetRate()
		{
			/* Start at last recorded point, end at latest when reaching the earliest recorded point */
			int point = this->prev_index;
			int last_point = this->next_index - this->num_valid;
			if (last_point < 0) last_point += NUM_FRAMERATE_POINTS;

			/* Number of data points collected */
			int count = 0;
			/* Time of previous data point */
			TimingMeasurement last = this->timestamps[point];
			/* Total duration covered by collected points */
			TimingMeasurement total = 0;

			/* We have nothing to compare the first point against */
			point--;
			if (point < 0) point = NUM_FRAMERATE_POINTS - 1;

			while (point != last_point) {
				/* Only record valid data points, but pretend the gaps in measurements aren't there */
				if (this->durations[point] != INVALID_DURATION) {
					total += last - this->timestamps[point];
					count++;
				}
				last = this->timestamps[point];
				if (total >= TIMESTAMP_PRECISION) break; // end after 1 second has been collected
				point--;
				if (point < 0) point = NUM_FRAMERATE_POINTS - 1;
			}

			if (total == 0 || count == 0) return 0;
			return (double)count * TIMESTAMP_PRECISION / total;
		}
	};

	/** %Game loop rate, cycles per second */
	static const double GL_RATE = 1000.0 / MILLISECONDS_PER_TICK;

	/**
	 * Storage for all performance element measurements.
	 * Elements are initialized with the expected rate in recorded values per second.
	 * @hideinitializer
	 */
	PerformanceData _pf_data[PFE_MAX] = {
		PerformanceData(GL_RATE),               // PFE_GAMELOOP
		PerformanceData(1),                     // PFE_ACC_GL_ECONOMY
		PerformanceData(1),                     // PFE_ACC_GL_TRAINS
		PerformanceData(1),                     // PFE_ACC_GL_ROADVEHS
		PerformanceData(1),                     // PFE_ACC_GL_SHIPS
		PerformanceData(1),                     // PFE_ACC_GL_AIRCRAFT
		PerformanceData(1),                     // PFE_GL_LANDSCAPE
		PerformanceData(1),                     // PFE_GL_LINKGRAPH
		PerformanceData(1000.0 / 30),           // PFE_DRAWING
		PerformanceData(1),                     // PFE_ACC_DRAWWORLD
		PerformanceData(60.0),                  // PFE_VIDEO
		PerformanceData(1000.0 * 8192 / 44100), // PFE_SOUND
		PerformanceData(1),                     // PFE_ALLSCRIPTS
		PerformanceData(1),                     // PFE_GAMESCRIPT
		PerformanceData(1),                     // PFE_AI0 ...
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),
		PerformanceData(1),                     // PFE_AI14
	};

}


/**
 * Return a timestamp with \c TIMESTAMP_PRECISION ticks per second precision.
 * The basis of the timestamp is implementation defined, but the value should be steady,
 * so differences can be taken to reliably measure intervals.
 */
static TimingMeasurement GetPerformanceTimer()
{
	using namespace std::chrono;
	return (TimingMeasurement)time_point_cast<microseconds>(high_resolution_clock::now()).time_since_epoch().count();
}


/**
 * Begin a cycle of a measured element.
 * @param elem The element to be measured
 */
PerformanceMeasurer::PerformanceMeasurer(PerformanceElement elem)
{
	assert(elem < PFE_MAX);

	this->elem = elem;
	this->start_time = GetPerformanceTimer();
}

/** Finish a cycle of a measured element and store the measurement taken. */
PerformanceMeasurer::~PerformanceMeasurer()
{
	if (this->elem == PFE_ALLSCRIPTS) {
		/* Hack to not record scripts total when no scripts are active */
		bool any_active = _pf_data[PFE_GAMESCRIPT].num_valid > 0;
		for (uint e = PFE_AI0; e < PFE_MAX; e++) any_active |= _pf_data[e].num_valid > 0;
		if (!any_active) {
			PerformanceMeasurer::SetInactive(PFE_ALLSCRIPTS);
			return;
		}
	}
	if (this->elem == PFE_SOUND) {
		/* PFE_SOUND measurements are made from the mixer thread.
		 * _pf_data cannot be concurrently accessed from the mixer thread
		 * and the main thread, so store the measurement results in a
		 * mutex-protected queue which is drained by the main thread.
		 * See: ProcessPendingPerformanceMeasurements() */
		TimingMeasurement end = GetPerformanceTimer();
		std::lock_guard lk(_sound_perf_lock);
		if (_sound_perf_measurements.size() >= NUM_FRAMERATE_POINTS * 2) return;
		_sound_perf_measurements.push_back(this->start_time);
		_sound_perf_measurements.push_back(end);
		_sound_perf_pending.store(true, std::memory_order_release);
		return;
	}
	_pf_data[this->elem].Add(this->start_time, GetPerformanceTimer());
}

/** Set the rate of expected cycles per second of a performance element. */
void PerformanceMeasurer::SetExpectedRate(double rate)
{
	_pf_data[this->elem].expected_rate = rate;
}

/** Mark a performance element as not currently in use. */
/* static */ void PerformanceMeasurer::SetInactive(PerformanceElement elem)
{
	_pf_data[elem].num_valid = 0;
	_pf_data[elem].next_index = 0;
	_pf_data[elem].prev_index = 0;
}

/**
 * Indicate that a cycle of "pause" where no processing occurs.
 * @param elem The element not currently being processed
 */
/* static */ void PerformanceMeasurer::Paused(PerformanceElement elem)
{
	PerformanceMeasurer::SetInactive(elem);
	_pf_data[elem].AddPause(GetPerformanceTimer());
}


/**
 * Begin measuring one block of the accumulating value.
 * @param elem The element to be measured
 */
PerformanceAccumulator::PerformanceAccumulator(PerformanceElement elem)
{
	assert(elem < PFE_MAX);

	this->elem = elem;
	this->start_time = GetPerformanceTimer();
}

/** Finish and add one block of the accumulating value. */
PerformanceAccumulator::~PerformanceAccumulator()
{
	_pf_data[this->elem].AddAccumulate(GetPerformanceTimer() - this->start_time);
}

/**
 * Store the previous accumulator value and reset for a new cycle of accumulating measurements.
 * @note This function must be called once per frame, otherwise measurements are not collected.
 * @param elem The element to begin a new measurement cycle of
 */
void PerformanceAccumulator::Reset(PerformanceElement elem)
{
	_pf_data[elem].BeginAccumulate(GetPerformanceTimer());
}


void ShowFrametimeGraphWindow(PerformanceElement elem);


static const PerformanceElement DISPLAY_ORDER_PFE[PFE_MAX] = {
	PFE_GAMELOOP,
	PFE_GL_ECONOMY,
	PFE_GL_TRAINS,
	PFE_GL_ROADVEHS,
	PFE_GL_SHIPS,
	PFE_GL_AIRCRAFT,
	PFE_GL_LANDSCAPE,
	PFE_ALLSCRIPTS,
	PFE_GAMESCRIPT,
	PFE_AI0,
	PFE_AI1,
	PFE_AI2,
	PFE_AI3,
	PFE_AI4,
	PFE_AI5,
	PFE_AI6,
	PFE_AI7,
	PFE_AI8,
	PFE_AI9,
	PFE_AI10,
	PFE_AI11,
	PFE_AI12,
	PFE_AI13,
	PFE_AI14,
	PFE_GL_LINKGRAPH,
	PFE_DRAWING,
	PFE_DRAWWORLD,
	PFE_VIDEO,
	PFE_SOUND,
};

static std::string_view GetAIName(int ai_index)
{
	if (!Company::IsValidAiID(ai_index)) return {};
	return Company::Get(ai_index)->ai_info->GetName();
}

/** @hideinitializer */
static constexpr NWidgetPart _framerate_window_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_FRW_CAPTION),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.frametext), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(WWT_TEXT, INVALID_COLOUR, WID_FRW_RATE_GAMELOOP), SetToolTip(STR_FRAMERATE_RATE_GAMELOOP_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
			NWidget(WWT_TEXT, INVALID_COLOUR, WID_FRW_RATE_DRAWING),  SetToolTip(STR_FRAMERATE_RATE_BLITTER_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
			NWidget(WWT_TEXT, INVALID_COLOUR, WID_FRW_RATE_FACTOR), SetToolTip(STR_FRAMERATE_SPEED_FACTOR_TOOLTIP), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY),
			NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.frametext), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
				NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FRW_TIMES_NAMES), SetScrollbar(WID_FRW_SCROLLBAR),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FRW_TIMES_CURRENT), SetScrollbar(WID_FRW_SCROLLBAR),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FRW_TIMES_AVERAGE), SetScrollbar(WID_FRW_SCROLLBAR),
					NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FRW_ALLOCSIZE), SetScrollbar(WID_FRW_SCROLLBAR),
				EndContainer(),
				NWidget(WWT_TEXT, INVALID_COLOUR, WID_FRW_INFO_DATA_POINTS), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_FRW_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_GREY),
		EndContainer(),
	EndContainer(),
};

struct FramerateWindow : Window {
	int num_active = 0;
	int num_displayed = 0;

	struct CachedDecimal {
		StringID strid;
		uint32_t value;

		inline void SetRate(double value, double target)
		{
			const double threshold_good = target * 0.95;
			const double threshold_bad = target * 2 / 3;
			this->value = (uint32_t)(value * 100);
			this->strid = (value > threshold_good) ? STR_FRAMERATE_FPS_GOOD : (value < threshold_bad) ? STR_FRAMERATE_FPS_BAD : STR_FRAMERATE_FPS_WARN;
		}

		inline void SetTime(double value, double target)
		{
			const double threshold_good = target / 3;
			const double threshold_bad = target;
			this->value = (uint32_t)(value * 100);
			this->strid = (value < threshold_good) ? STR_FRAMERATE_MS_GOOD : (value > threshold_bad) ? STR_FRAMERATE_MS_BAD : STR_FRAMERATE_MS_WARN;
		}

		inline uint32_t GetValue() const { return this->value; }
		inline uint32_t GetDecimals() const { return 2; }
	};

	CachedDecimal rate_gameloop{}; ///< cached game loop tick rate
	CachedDecimal rate_drawing{}; ///< cached drawing frame rate
	CachedDecimal speed_gameloop{}; ///< cached game loop speed factor
	std::array<CachedDecimal, PFE_MAX> times_shortterm{}; ///< cached short term average times
	std::array<CachedDecimal, PFE_MAX> times_longterm{}; ///< cached long term average times

	static constexpr int MIN_ELEMENTS = 5; ///< smallest number of elements to display

	FramerateWindow(WindowDesc &desc, WindowNumber number) : Window(desc)
	{
		this->InitNested(number);
		this->UpdateData();
		this->num_displayed = this->num_active;

		/* Window is always initialised to MIN_ELEMENTS height, resize to contain num_displayed */
		ResizeWindow(this, 0, (std::max(MIN_ELEMENTS, this->num_displayed) - MIN_ELEMENTS) * GetCharacterHeight(FS_NORMAL));
	}

	/** Update the window on a regular interval. */
	const IntervalTimer<TimerWindow> update_interval = {std::chrono::milliseconds(100), [this](auto) {
		this->UpdateData();
		this->SetDirty();
	}};

	void UpdateData()
	{
		double gl_rate = _pf_data[PFE_GAMELOOP].GetRate();
		this->rate_gameloop.SetRate(gl_rate, _pf_data[PFE_GAMELOOP].expected_rate);
		this->speed_gameloop.SetRate(gl_rate / _pf_data[PFE_GAMELOOP].expected_rate, 1.0);
		if (this->IsShaded()) return; // in small mode, this is everything needed

		this->rate_drawing.SetRate(_pf_data[PFE_DRAWING].GetRate(), _settings_client.gui.refresh_rate);

		int new_active = 0;
		for (PerformanceElement e = PFE_FIRST; e < PFE_MAX; e++) {
			this->times_shortterm[e].SetTime(_pf_data[e].GetAverageDurationMilliseconds(8), MILLISECONDS_PER_TICK);
			this->times_longterm[e].SetTime(_pf_data[e].GetAverageDurationMilliseconds(NUM_FRAMERATE_POINTS), MILLISECONDS_PER_TICK);
			if (_pf_data[e].num_valid > 0) {
				new_active++;
			}
		}

		if (new_active != this->num_active) {
			this->num_active = new_active;
			Scrollbar *sb = this->GetScrollbar(WID_FRW_SCROLLBAR);
			sb->SetCount(this->num_active);
			sb->SetCapacity(std::min(this->num_displayed, this->num_active));
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_FRW_CAPTION:
				/* When the window is shaded, the caption shows game loop rate and speed factor */
				if (!this->IsShaded()) return GetString(STR_FRAMERATE_CAPTION);

				return GetString(STR_FRAMERATE_CAPTION_SMALL, this->rate_gameloop.strid, this->rate_gameloop.GetValue(), this->rate_gameloop.GetDecimals(), this->speed_gameloop.GetValue(), this->speed_gameloop.GetDecimals());

			case WID_FRW_RATE_GAMELOOP:
				return GetString(STR_FRAMERATE_RATE_GAMELOOP, this->rate_gameloop.strid, this->rate_gameloop.GetValue(), this->rate_gameloop.GetDecimals());

			case WID_FRW_RATE_DRAWING:
				return GetString(STR_FRAMERATE_RATE_BLITTER, this->rate_drawing.strid, this->rate_drawing.GetValue(), this->rate_drawing.GetDecimals());

			case WID_FRW_RATE_FACTOR:
				return GetString(STR_FRAMERATE_SPEED_FACTOR, this->speed_gameloop.GetValue(), this->speed_gameloop.GetDecimals());

			case WID_FRW_INFO_DATA_POINTS:
				return GetString(STR_FRAMERATE_DATA_POINTS, NUM_FRAMERATE_POINTS);

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_FRW_RATE_GAMELOOP:
				size = GetStringBoundingBox(GetString(STR_FRAMERATE_RATE_GAMELOOP, STR_FRAMERATE_FPS_GOOD, GetParamMaxDigits(6), 2));
				break;
			case WID_FRW_RATE_DRAWING:
				size = GetStringBoundingBox(GetString(STR_FRAMERATE_RATE_BLITTER, STR_FRAMERATE_FPS_GOOD, GetParamMaxDigits(6), 2));
				break;
			case WID_FRW_RATE_FACTOR:
				size = GetStringBoundingBox(GetString(STR_FRAMERATE_SPEED_FACTOR, GetParamMaxDigits(6), 2));
				break;

			case WID_FRW_TIMES_NAMES: {
				size.width = 0;
				size.height = GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal + MIN_ELEMENTS * GetCharacterHeight(FS_NORMAL);
				resize.width = 0;
				fill.height = resize.height = GetCharacterHeight(FS_NORMAL);
				for (PerformanceElement e : DISPLAY_ORDER_PFE) {
					if (_pf_data[e].num_valid == 0) continue;
					Dimension line_size;
					if (e < PFE_AI0) {
						line_size = GetStringBoundingBox(STR_FRAMERATE_GAMELOOP + e);
					} else {
						line_size = GetStringBoundingBox(GetString(STR_FRAMERATE_AI, e - PFE_AI0 + 1, GetAIName(e - PFE_AI0)));
					}
					size.width = std::max(size.width, line_size.width);
				}
				break;
			}

			case WID_FRW_TIMES_CURRENT:
			case WID_FRW_TIMES_AVERAGE:
			case WID_FRW_ALLOCSIZE: {
				size = GetStringBoundingBox(STR_FRAMERATE_CURRENT + (widget - WID_FRW_TIMES_CURRENT));
				Dimension item_size = GetStringBoundingBox(GetString(STR_FRAMERATE_MS_GOOD, GetParamMaxDigits(6), 2));
				size.width = std::max(size.width, item_size.width);
				size.height += GetCharacterHeight(FS_NORMAL) * MIN_ELEMENTS + WidgetDimensions::scaled.vsep_normal;
				resize.width = 0;
				fill.height = resize.height = GetCharacterHeight(FS_NORMAL);
				break;
			}
		}
	}

	/** Render a column of formatted average durations */
	void DrawElementTimesColumn(const Rect &r, StringID heading_str, std::span<const CachedDecimal> values) const
	{
		const Scrollbar *sb = this->GetScrollbar(WID_FRW_SCROLLBAR);
		int32_t skip = sb->GetPosition();
		int drawable = this->num_displayed;
		int y = r.top;
		DrawString(r.left, r.right, y, heading_str, TC_FROMSTRING, SA_CENTER, true);
		y += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;
		for (PerformanceElement e : DISPLAY_ORDER_PFE) {
			if (_pf_data[e].num_valid == 0) continue;
			if (skip > 0) {
				skip--;
			} else {
				DrawString(r.left, r.right, y, GetString(values[e].strid, values[e].GetValue(), values[e].GetDecimals()), TC_FROMSTRING, SA_RIGHT | SA_FORCE);
				y += GetCharacterHeight(FS_NORMAL);
				drawable--;
				if (drawable == 0) break;
			}
		}
	}

	void DrawElementAllocationsColumn(const Rect &r) const
	{
		const Scrollbar *sb = this->GetScrollbar(WID_FRW_SCROLLBAR);
		int32_t skip = sb->GetPosition();
		int drawable = this->num_displayed;
		int y = r.top;
		DrawString(r.left, r.right, y, STR_FRAMERATE_MEMORYUSE, TC_FROMSTRING, SA_CENTER, true);
		y += GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal;
		for (PerformanceElement e : DISPLAY_ORDER_PFE) {
			if (_pf_data[e].num_valid == 0) continue;
			if (skip > 0) {
				skip--;
			} else if (e == PFE_GAMESCRIPT || e >= PFE_AI0) {
				uint64_t value = e == PFE_GAMESCRIPT ? Game::GetInstance()->GetAllocatedMemory() : Company::Get(e - PFE_AI0)->ai_instance->GetAllocatedMemory();
				DrawString(r.left, r.right, y, GetString(STR_FRAMERATE_BYTES_GOOD, value), TC_FROMSTRING, SA_RIGHT | SA_FORCE);
				y += GetCharacterHeight(FS_NORMAL);
				drawable--;
				if (drawable == 0) break;
			} else if (e == PFE_SOUND) {
				DrawString(r.left, r.right, y, GetString(STR_FRAMERATE_BYTES_GOOD, GetSoundPoolAllocatedMemory()), TC_FROMSTRING, SA_RIGHT | SA_FORCE);
				y += GetCharacterHeight(FS_NORMAL);
				drawable--;
				if (drawable == 0) break;
			} else {
				/* skip non-script */
				y += GetCharacterHeight(FS_NORMAL);
				drawable--;
				if (drawable == 0) break;
			}
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_FRW_TIMES_NAMES: {
				/* Render a column of titles for performance element names */
				const Scrollbar *sb = this->GetScrollbar(WID_FRW_SCROLLBAR);
				int32_t skip = sb->GetPosition();
				int drawable = this->num_displayed;
				int y = r.top + GetCharacterHeight(FS_NORMAL) + WidgetDimensions::scaled.vsep_normal; // first line contains headings in the value columns
				for (PerformanceElement e : DISPLAY_ORDER_PFE) {
					if (_pf_data[e].num_valid == 0) continue;
					if (skip > 0) {
						skip--;
					} else {
						if (e < PFE_AI0) {
							DrawString(r.left, r.right, y, STR_FRAMERATE_GAMELOOP + e, TC_FROMSTRING, SA_LEFT);
						} else {
							DrawString(r.left, r.right, y, GetString(STR_FRAMERATE_AI, e - PFE_AI0 + 1, GetAIName(e - PFE_AI0)), TC_FROMSTRING, SA_LEFT);
						}
						y += GetCharacterHeight(FS_NORMAL);
						drawable--;
						if (drawable == 0) break;
					}
				}
				break;
			}
			case WID_FRW_TIMES_CURRENT:
				/* Render short-term average values */
				DrawElementTimesColumn(r, STR_FRAMERATE_CURRENT, this->times_shortterm);
				break;
			case WID_FRW_TIMES_AVERAGE:
				/* Render averages of all recorded values */
				DrawElementTimesColumn(r, STR_FRAMERATE_AVERAGE, this->times_longterm);
				break;
			case WID_FRW_ALLOCSIZE:
				DrawElementAllocationsColumn(r);
				break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_FRW_TIMES_NAMES:
			case WID_FRW_TIMES_CURRENT:
			case WID_FRW_TIMES_AVERAGE: {
				/* Open time graph windows when clicking detail measurement lines */
				const Scrollbar *sb = this->GetScrollbar(WID_FRW_SCROLLBAR);
				int32_t line = sb->GetScrolledRowFromWidget(pt.y, this, widget, WidgetDimensions::scaled.vsep_normal + GetCharacterHeight(FS_NORMAL));
				if (line != INT32_MAX) {
					line++;
					/* Find the visible line that was clicked */
					for (PerformanceElement e : DISPLAY_ORDER_PFE) {
						if (_pf_data[e].num_valid > 0) line--;
						if (line == 0) {
							ShowFrametimeGraphWindow(e);
							break;
						}
					}
				}
				break;
			}
		}
	}

	void OnResize() override
	{
		auto *wid = this->GetWidget<NWidgetResizeBase>(WID_FRW_TIMES_NAMES);
		this->num_displayed = (wid->current_y - wid->min_y - WidgetDimensions::scaled.vsep_normal) / GetCharacterHeight(FS_NORMAL) - 1; // subtract 1 for headings
		this->GetScrollbar(WID_FRW_SCROLLBAR)->SetCapacity(this->num_displayed);
	}
};

static WindowDesc _framerate_display_desc(
	WDP_AUTO, "framerate_display", 0, 0,
	WC_FRAMERATE_DISPLAY, WC_NONE,
	{},
	_framerate_window_widgets
);


/** @hideinitializer */
static constexpr NWidgetPart _frametime_graph_window_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_FGW_CAPTION), SetTextStyle(TC_WHITE),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.frametext),
			NWidget(WWT_EMPTY, INVALID_COLOUR, WID_FGW_GRAPH),
		EndContainer(),
	EndContainer(),
};

struct FrametimeGraphWindow : Window {
	int vertical_scale = TIMESTAMP_PRECISION / 10; ///< number of TIMESTAMP_PRECISION units vertically
	int horizontal_scale = 4; ///< number of half-second units horizontally

	PerformanceElement element{}; ///< what element this window renders graph for
	Dimension graph_size{}; ///< size of the main graph area (excluding axis labels)

	FrametimeGraphWindow(WindowDesc &desc, WindowNumber number) : Window(desc), element(static_cast<PerformanceElement>(number))
	{
		this->InitNested(number);
		this->UpdateScale();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_FGW_CAPTION:
				if (this->element < PFE_AI0) {
					return GetString(STR_FRAMETIME_CAPTION_GAMELOOP + this->element);
				}
				return GetString(STR_FRAMETIME_CAPTION_AI, this->element - PFE_AI0 + 1, GetAIName(this->element - PFE_AI0));

			default:
				return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget == WID_FGW_GRAPH) {
			Dimension size_ms_label = GetStringBoundingBox(GetString(STR_FRAMERATE_GRAPH_MILLISECONDS, 100));
			Dimension size_s_label = GetStringBoundingBox(GetString(STR_FRAMERATE_GRAPH_SECONDS, 100));

			/* Size graph in height to fit at least 10 vertical labels with space between, or at least 100 pixels */
			graph_size.height = std::max<uint>(ScaleGUITrad(100), 10 * (size_ms_label.height + WidgetDimensions::scaled.vsep_normal));
			/* Always 2:1 graph area */
			graph_size.width = 2 * graph_size.height;
			size = graph_size;

			size.width += size_ms_label.width + WidgetDimensions::scaled.hsep_normal;
			size.height += size_s_label.height + WidgetDimensions::scaled.vsep_normal;
		}
	}

	void SelectHorizontalScale(TimingMeasurement range)
	{
		/* 60 Hz graphical drawing results in a value of approximately TIMESTAMP_PRECISION,
		 * this lands exactly on the scale = 2 vs scale = 4 boundary.
		 * To avoid excessive switching of the horizontal scale, bias these performance
		 * categories away from this scale boundary. */
		if (this->element == PFE_DRAWING || this->element == PFE_DRAWWORLD) range += (range / 2);

		/* Determine horizontal scale based on period covered by 60 points
		 * (slightly less than 2 seconds at full game speed) */
		struct ScaleDef { TimingMeasurement range; int scale; };
		static const std::initializer_list<ScaleDef> hscales = {
			{ TIMESTAMP_PRECISION * 120, 60 },
			{ TIMESTAMP_PRECISION *  10, 20 },
			{ TIMESTAMP_PRECISION *   5, 10 },
			{ TIMESTAMP_PRECISION *   3,  4 },
			{ TIMESTAMP_PRECISION *   1,  2 },
		};
		for (const auto &sc : hscales) {
			if (range < sc.range) this->horizontal_scale = sc.scale;
		}
	}

	void SelectVerticalScale(TimingMeasurement range)
	{
		/* Determine vertical scale based on peak value (within the horizontal scale + a bit) */
		static const std::initializer_list<TimingMeasurement> vscales = {
			TIMESTAMP_PRECISION * 100,
			TIMESTAMP_PRECISION * 10,
			TIMESTAMP_PRECISION * 5,
			TIMESTAMP_PRECISION,
			TIMESTAMP_PRECISION / 2,
			TIMESTAMP_PRECISION / 5,
			TIMESTAMP_PRECISION / 10,
			TIMESTAMP_PRECISION / 50,
			TIMESTAMP_PRECISION / 200,
		};
		for (const auto &sc : vscales) {
			if (range < sc) this->vertical_scale = (int)sc;
		}
	}

	/** Recalculate the graph scaling factors based on current recorded data */
	void UpdateScale()
	{
		const auto &durations = _pf_data[this->element].durations;
		const auto &timestamps = _pf_data[this->element].timestamps;
		int num_valid = _pf_data[this->element].num_valid;
		int point = _pf_data[this->element].prev_index;

		TimingMeasurement lastts = timestamps[point];
		TimingMeasurement time_sum = 0;
		TimingMeasurement peak_value = 0;
		int count = 0;

		/* Sensible default for when too few measurements are available */
		this->horizontal_scale = 4;

		for (int i = 1; i < num_valid; i++) {
			point--;
			if (point < 0) point = NUM_FRAMERATE_POINTS - 1;

			TimingMeasurement value = durations[point];
			if (value == PerformanceData::INVALID_DURATION) {
				/* Skip gaps in data by pretending time is continuous across them */
				lastts = timestamps[point];
				continue;
			}
			if (value > peak_value) peak_value = value;
			count++;

			/* Accumulate period of time covered by data */
			time_sum += lastts - timestamps[point];
			lastts = timestamps[point];

			/* Enough data to select a range and get decent data density */
			if (count == 60) this->SelectHorizontalScale(time_sum);

			/* End when enough points have been collected and the horizontal scale has been exceeded */
			if (count >= 60 && time_sum >= (this->horizontal_scale + 2) * TIMESTAMP_PRECISION / 2) break;
		}

		this->SelectVerticalScale(peak_value);
	}

	/** Update the scaling on a regular interval. */
	const IntervalTimer<TimerWindow> update_interval = {std::chrono::milliseconds(500), [this](auto) {
		this->UpdateScale();
	}};

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		this->SetDirty();
	}

	/** Scale and interpolate a value from a source range into a destination range */
	template <typename T>
	static inline T Scinterlate(T dst_min, T dst_max, T src_min, T src_max, T value)
	{
		T dst_diff = dst_max - dst_min;
		T src_diff = src_max - src_min;
		return (value - src_min) * dst_diff / src_diff + dst_min;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget == WID_FGW_GRAPH) {
			const auto &durations  = _pf_data[this->element].durations;
			const auto &timestamps = _pf_data[this->element].timestamps;
			int point = _pf_data[this->element].prev_index;

			const int x_zero = r.right - (int)this->graph_size.width;
			const int x_max = r.right;
			const int y_zero = r.top + (int)this->graph_size.height;
			const int y_max = r.top;
			const PixelColour c_grid = PC_DARK_GREY;
			const PixelColour c_lines = PC_BLACK;
			const PixelColour c_peak = PC_DARK_RED;

			const TimingMeasurement draw_horz_scale = (TimingMeasurement)this->horizontal_scale * TIMESTAMP_PRECISION / 2;
			const TimingMeasurement draw_vert_scale = (TimingMeasurement)this->vertical_scale;

			/* Number of \c horizontal_scale units in each horizontal division */
			const uint horz_div_scl = (this->horizontal_scale <= 20) ? 1 : 10;
			/* Number of divisions of the horizontal axis */
			const uint horz_divisions = this->horizontal_scale / horz_div_scl;
			/* Number of divisions of the vertical axis */
			const uint vert_divisions = 10;

			/* Draw division lines and labels for the vertical axis */
			for (uint division = 0; division < vert_divisions; division++) {
				int y = Scinterlate(y_zero, y_max, 0, (int)vert_divisions, (int)division);
				GfxDrawLine(x_zero, y, x_max, y, c_grid);
				if (division % 2 == 0) {
					if ((TimingMeasurement)this->vertical_scale > TIMESTAMP_PRECISION) {
						DrawString(r.left, x_zero - WidgetDimensions::scaled.hsep_normal, y - GetCharacterHeight(FS_SMALL),
							GetString(STR_FRAMERATE_GRAPH_SECONDS, this->vertical_scale * division / 10 / TIMESTAMP_PRECISION),
							TC_GREY, SA_RIGHT | SA_FORCE, false, FS_SMALL);
					} else {
						DrawString(r.left, x_zero - WidgetDimensions::scaled.hsep_normal, y - GetCharacterHeight(FS_SMALL),
							GetString(STR_FRAMERATE_GRAPH_MILLISECONDS, this->vertical_scale * division / 10 * 1000 / TIMESTAMP_PRECISION),
							TC_GREY, SA_RIGHT | SA_FORCE, false, FS_SMALL);
					}
				}
			}
			/* Draw division lines and labels for the horizontal axis */
			for (uint division = horz_divisions; division > 0; division--) {
				int x = Scinterlate(x_zero, x_max, 0, (int)horz_divisions, (int)horz_divisions - (int)division);
				GfxDrawLine(x, y_max, x, y_zero, c_grid);
				if (division % 2 == 0) {
					DrawString(x, x_max, y_zero + WidgetDimensions::scaled.vsep_normal,
						GetString(STR_FRAMERATE_GRAPH_SECONDS, division * horz_div_scl / 2),
						TC_GREY, SA_LEFT | SA_FORCE, false, FS_SMALL);
				}
			}

			/* Position of last rendered data point */
			Point lastpoint = {
				x_max,
				(int)Scinterlate<int64_t>(y_zero, y_max, 0, this->vertical_scale, durations[point])
			};
			/* Timestamp of last rendered data point */
			TimingMeasurement lastts = timestamps[point];

			TimingMeasurement peak_value = 0;
			Point peak_point = { 0, 0 };
			TimingMeasurement value_sum = 0;
			TimingMeasurement time_sum = 0;
			int points_drawn = 0;

			for (int i = 1; i < NUM_FRAMERATE_POINTS; i++) {
				point--;
				if (point < 0) point = NUM_FRAMERATE_POINTS - 1;

				TimingMeasurement value = durations[point];
				if (value == PerformanceData::INVALID_DURATION) {
					/* Skip gaps in measurements, pretend the data points on each side are continuous */
					lastts = timestamps[point];
					continue;
				}

				/* Use total time period covered for value along horizontal axis */
				time_sum += lastts - timestamps[point];
				lastts = timestamps[point];
				/* Stop if past the width of the graph */
				if (time_sum > draw_horz_scale) break;

				/* Draw line from previous point to new point */
				Point newpoint = {
					(int)Scinterlate<int64_t>(x_zero, x_max, 0, (int64_t)draw_horz_scale, (int64_t)draw_horz_scale - (int64_t)time_sum),
					(int)Scinterlate<int64_t>(y_zero, y_max, 0, (int64_t)draw_vert_scale, (int64_t)value)
				};
				if (newpoint.x > lastpoint.x) continue; // don't draw backwards
				GfxDrawLine(lastpoint.x, lastpoint.y, newpoint.x, newpoint.y, c_lines);
				lastpoint = newpoint;

				/* Record peak and average value across graphed data */
				value_sum += value;
				points_drawn++;
				if (value > peak_value) {
					peak_value = value;
					peak_point = newpoint;
				}
			}

			/* If the peak value is significantly larger than the average, mark and label it */
			if (points_drawn > 0 && peak_value > TIMESTAMP_PRECISION / 100 && 2 * peak_value > 3 * value_sum / points_drawn) {
				TextColour tc_peak = c_peak.ToTextColour();
				GfxFillRect(peak_point.x - 1, peak_point.y - 1, peak_point.x + 1, peak_point.y + 1, c_peak);
				uint64_t value = peak_value * 1000 / TIMESTAMP_PRECISION;
				int label_y = std::max(y_max, peak_point.y - GetCharacterHeight(FS_SMALL));
				if (peak_point.x - x_zero > (int)this->graph_size.width / 2) {
					DrawString(x_zero, peak_point.x - WidgetDimensions::scaled.hsep_normal, label_y, GetString(STR_FRAMERATE_GRAPH_MILLISECONDS, value), tc_peak, SA_RIGHT | SA_FORCE, false, FS_SMALL);
				} else {
					DrawString(peak_point.x + WidgetDimensions::scaled.hsep_normal, x_max, label_y, GetString(STR_FRAMERATE_GRAPH_MILLISECONDS, value), tc_peak, SA_LEFT | SA_FORCE, false, FS_SMALL);
				}
			}
		}
	}
};

static WindowDesc _frametime_graph_window_desc(
	WDP_AUTO, "frametime_graph", 140, 90,
	WC_FRAMETIME_GRAPH, WC_NONE,
	{},
	_frametime_graph_window_widgets
);



/** Open the general framerate window */
void ShowFramerateWindow()
{
	AllocateWindowDescFront<FramerateWindow>(_framerate_display_desc, 0);
}

/** Open a graph window for a performance element */
void ShowFrametimeGraphWindow(PerformanceElement elem)
{
	if (elem < PFE_FIRST || elem >= PFE_MAX) return; // maybe warn?
	AllocateWindowDescFront<FrametimeGraphWindow>(_frametime_graph_window_desc, elem);
}

/** Print performance statistics to game console */
void ConPrintFramerate()
{
	const int count1 = NUM_FRAMERATE_POINTS / 8;
	const int count2 = NUM_FRAMERATE_POINTS / 4;
	const int count3 = NUM_FRAMERATE_POINTS / 1;

	IConsolePrint(TC_SILVER, "Based on num. data points: {} {} {}", count1, count2, count3);

	static const std::array<std::string_view, PFE_MAX> MEASUREMENT_NAMES = {
		"Game loop",
		"  GL station ticks",
		"  GL train ticks",
		"  GL road vehicle ticks",
		"  GL ship ticks",
		"  GL aircraft ticks",
		"  GL landscape ticks",
		"  GL link graph delays",
		"Drawing",
		"  Viewport drawing",
		"Video output",
		"Sound mixing",
		"AI/GS scripts total",
		"Game script",
	};
	std::string ai_name_buf;

	bool printed_anything = false;

	for (const auto &e : { PFE_GAMELOOP, PFE_DRAWING, PFE_VIDEO }) {
		auto &pf = _pf_data[e];
		if (pf.num_valid == 0) continue;
		IConsolePrint(TC_GREEN, "{} rate: {:.2f}fps  (expected: {:.2f}fps)",
			MEASUREMENT_NAMES[e],
			pf.GetRate(),
			pf.expected_rate);
		printed_anything = true;
	}

	for (PerformanceElement e = PFE_FIRST; e < PFE_MAX; e++) {
		auto &pf = _pf_data[e];
		if (pf.num_valid == 0) continue;
		std::string_view name;
		if (e < PFE_AI0) {
			name = MEASUREMENT_NAMES[e];
		} else {
			ai_name_buf = fmt::format("AI {} {}", e - PFE_AI0 + 1, GetAIName(e - PFE_AI0));
			name = ai_name_buf;
		}
		IConsolePrint(TC_LIGHT_BLUE, "{} times: {:.2f}ms  {:.2f}ms  {:.2f}ms",
			name,
			pf.GetAverageDurationMilliseconds(count1),
			pf.GetAverageDurationMilliseconds(count2),
			pf.GetAverageDurationMilliseconds(count3));
		printed_anything = true;
	}

	if (!printed_anything) {
		IConsolePrint(CC_ERROR, "No performance measurements have been taken yet.");
	}
}

/**
 * This drains the PFE_SOUND measurement data queue into _pf_data.
 * PFE_SOUND measurements are made by the mixer thread and so cannot be stored
 * into _pf_data directly, because this would not be thread safe and would violate
 * the invariants of the FPS and frame graph windows.
 * @see PerformanceMeasurement::~PerformanceMeasurement()
 */
void ProcessPendingPerformanceMeasurements()
{
	if (_sound_perf_pending.load(std::memory_order_acquire)) {
		std::lock_guard lk(_sound_perf_lock);
		for (size_t i = 0; i < _sound_perf_measurements.size(); i += 2) {
			_pf_data[PFE_SOUND].Add(_sound_perf_measurements[i], _sound_perf_measurements[i + 1]);
		}
		_sound_perf_measurements.clear();
		_sound_perf_pending.store(false, std::memory_order_relaxed);
	}
}
