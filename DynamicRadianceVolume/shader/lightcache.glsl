// Options with effects on multiple shaders - most are wired as compile option into the program.

// Diffuse lighting mode.
//#define INDDIFFUSE_VIA_SH1
//#define INDDIFFUSE_VIA_SH2
//#define INDDIFFUSE_VIA_H 4 // 6

// Enable cascade transitions.
//#define ADDRESSVOL_CASCADE_TRANSITIONS

// Enable specular lighting.
//#define INDIRECT_SPECULAR


#define LIGHTCACHEMODE_CREATE 0
#define LIGHTCACHEMODE_LIGHT 1
#define LIGHTCACHEMODE_APPLY 2

#if LIGHTCACHEMODE == LIGHTCACHEMODE_CREATE
	#define LIGHTCACHE_BUFFER_MODIFIER restrict writeonly
	#define LIGHTCACHE_COUNTER_MODIFIER restrict coherent
#elif LIGHTCACHEMODE == LIGHTCACHEMODE_LIGHT
	#define LIGHTCACHE_BUFFER_MODIFIER restrict
	#define LIGHTCACHE_COUNTER_MODIFIER restrict readonly
#elif LIGHTCACHEMODE == LIGHTCACHEMODE_APPLY
	#define LIGHTCACHE_BUFFER_MODIFIER restrict readonly
	#define LIGHTCACHE_COUNTER_MODIFIER restrict readonly
#else
	#error "Please specify LIGHTCACHEMODE!"
#endif


struct LightCacheEntry
{
	vec3 Position; // Consider storing packed identifier!
	float _padding0;


#if defined(INDDIFFUSE_VIA_SH1) || defined(INDDIFFUSE_VIA_SH2)
	// Irradiance via SH (band, coefficient)
	// Consider packing!
	vec3 SH1neg1;
	float SH00_r;
	vec3 SH10;
	float SH00_g;
	vec3 SH1pos1;
	float SH00_b;

	#ifdef INDDIFFUSE_VIA_SH2
	vec3 SH2neg2;
	float SH20_r;
	vec3 SH2neg1;
	float SH20_g;
	vec3 SH2pos1;
	float SH20_b;
	vec3 SH2pos2;
	float _padding1;
	#endif

#elif defined(INDDIFFUSE_VIA_H)
	// Irradiance via H basis - in "Cache Local View Space"
	vec3 irradianceH1;
	float irradianceH4r;
	vec3 irradianceH2;
	float irradianceH4g;
	vec3 irradianceH3;
	float irradianceH4b;
	#if INDDIFFUSE_VIA_H > 4
	vec3 irradianceH5;
	float _padding1;
	vec3 irradianceH6;
	float _padding2;
	#endif
#endif

};

layout(std430, binding = 0) LIGHTCACHE_BUFFER_MODIFIER buffer LightCacheBuffer
{
	LightCacheEntry[] LightCacheEntries;
};

layout(std430, binding = 1) LIGHTCACHE_COUNTER_MODIFIER buffer LightCacheCounter
{
	uint NumCacheLightingThreadGroupsX; // Should be (TotalLightCacheCount + LIGHTING_THREADS_PER_GROUP - 1) / LIGHTING_THREADS_PER_GROUP
    uint NumCacheLightingThreadGroupsY; // Should be 1
    uint NumCacheLightingThreadGroupsZ; // Should be 1

    int TotalLightCacheCount;
};




#define LIGHTING_THREADS_PER_GROUP 512


mat3 ComputeLocalViewSpace(vec3 worldPosition)
{
	mat3 localViewSpace;
	localViewSpace[2] = normalize(CameraPosition - worldPosition); // Z
	localViewSpace[0] = normalize(vec3(localViewSpace[2].z, 0.0, -localViewSpace[2].x)); // X
	localViewSpace[1] = cross(localViewSpace[2], localViewSpace[0]); // Y

	return localViewSpace;
}

// Computes address volume cascade for a given worldPosition. If none fits, returns highest cascade (NumAddressVolumeCascades-1)
int ComputeAddressVolumeCascade(vec3 worldPosition)
{
	int addressVolumeCascade = 0;
	for(; addressVolumeCascade<NumAddressVolumeCascades-1; ++addressVolumeCascade)
	{
		if(all(lessThanEqual(worldPosition, AddressVolumeCascades[addressVolumeCascade].DecisionMax) &&
			greaterThanEqual(worldPosition, AddressVolumeCascades[addressVolumeCascade].DecisionMin)))
		{
			break;
		}
	}

	return addressVolumeCascade;
}

/// Returns 0.0 if its not in transition area. 1.0 max transition to NEXT cascade.
float ComputeAddressVolumeCascadeTransition(vec3 worldPosition, int addressVolumeCascade)
{
	vec3 distToMax = AddressVolumeCascades[addressVolumeCascade].DecisionMax - worldPosition;
	vec3 distToMin = worldPosition - AddressVolumeCascades[addressVolumeCascade].DecisionMin;

	float minDist = min(min(min(distToMax.x, distToMax.y), distToMax.z),
						min(min(distToMin.x, distToMin.y), distToMin.z));

	return clamp(1.0 - minDist / AddressVolumeCascades[addressVolumeCascade].WorldVoxelSize, 0.0, 1.0);
}