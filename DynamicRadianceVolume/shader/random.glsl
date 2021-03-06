uint WangHash(uint seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	return seed;
}

uint RandomUInt(inout uint seed)
{
	// Xorshift32
	seed ^= (seed << 13);
	seed ^= (seed >> 17);
	seed ^= (seed << 5);
	
	return seed;
}

float Random(inout uint seed)
{
	return float(RandomUInt(seed) % 8388593) / 8388593.0;
}

float RandomWang(inout uint seed)
{
	return float(WangHash(seed) % 8388593) / 8388593.0;
}

vec2 Random2(inout uint seed)
{
	return vec2(Random(seed), Random(seed));
}
