#if !defined(_TIMER_H)
#define _TIMER_H

#define AVERAGE_SAMPLES 128

#define Timer_Paused	0x0001

void Timer_InitModule();
void Timer_DeinitModule(); 

class Timer
{
public:
	void Init();

	void Update();
	void Reset();

	inline void SetRate(double _rate)	{ rate=_rate; }
	inline double GetRate()				{ return rate; }

	inline double TimeDeltaD()			{ return deltaD; }
	inline float TimeDeltaF()			{ return deltaF; }

	inline float GetFPS()				{ return (float)FPS; }

	inline double GetSecondsD()			{ return (double)accumulator/freq; }
	inline float GetSecondsF()			{ return (float)accumulator/freq; }

	void Pause(bool pause);
	inline bool IsPaused()				{ return (flags&Timer_Paused)?true:false; }

	inline void SetFixed(int fps)		{ fixed=true; fixedFPS=fps; }
	inline void SetFree()				{ fixed=false; }

protected:
	uint64 accumulator, lastUpdate, thisCall, lastCall, freq;

	double FPS, rate;
	int current;

	float deltaF;
	double deltaD;

	double history[AVERAGE_SAMPLES];
	static int avgSamples;


	uint32 flags;

	bool fixed;
	int fixedFPS;
};

#endif //_TIMER_H
