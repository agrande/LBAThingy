#include "LBAPlanetCulling.h"

#include "SceneView.h"

namespace
{
	TAutoConsoleVariable<int32> CVarGlobeBentCulling(TEXT("lba.Globe.BentCulling"), 1,
		TEXT("0: the renderer culls by the flat world, as without the bend (to compare: far islands seen from high up disappear)."));
	TAutoConsoleVariable<float> CVarGlobeCullBelow(TEXT("lba.Globe.CullBelow"), 40.f,
		TEXT("How deep below the sea things are still seen on the planet (m): the bent planet hides what is beyond."));
	TAutoConsoleVariable<float> CVarGlobeCullAbove(TEXT("lba.Globe.CullAbove"), 100.f,
		TEXT("How high above the sea the planet's things go (m): what the bend can bring into view is looked for up to there."));

	/** View rays per side over the view, and samples along each ray's bent part. */
	constexpr int32 Rays = 17;
	constexpr int32 Samples = 32;
	/** Margins for what falls between the samples. */
	constexpr double AngleMargin = UE_DOUBLE_PI / 180.0;
	constexpr double ReachMargin = 1000.0;
}

FLBAPlanetViewExtension::FLBAPlanetViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FLBAPlanetViewExtension::SetBend(const FVector& InView, double InRadius, double InFlatRadius, double InSeaLevel, bool bInBent)
{
	View = InView;
	Radius = InRadius;
	FlatRadius = FMath::Max(InFlatRadius, 0.0);
	SeaLevel = InSeaLevel;
	bBent = bInBent;
}

bool FLBAPlanetViewExtension::IsCulling() const
{
	return bBent && Radius > 0.0 && CVarGlobeBentCulling.GetValueOnGameThread() != 0;
}

void FLBAPlanetViewExtension::SetupViewFamily(FSceneViewFamily& InViewFamily)
{
	// Occlusion is tested with flat bounds against the depth of the bent view: far islands sink behind the sea in
	// front of where their flat bounds are, and were hidden by it.
	if (IsCulling())
	{
		InViewFamily.EngineShowFlags.SetDisableOcclusionQueries(true);
	}
}

template <typename FVisit>
void FLBAPlanetViewExtension::ForEachFlatPlace(const FVector& Direction, FVisit&& Visit) const
{
	// Inside the flat circle the ray shows the flat world as it is.
	Visit(Direction);

	const double Across = Direction.Size2D();
	if (Across < UE_DOUBLE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	// Past it, in the ray's vertical plane: the bend's sphere is centered below the flat circle's edge, the camera at
	// Up above that center and the ray at (t Across - FlatRadius, Up + t Direction.Z) from it. A flat place at a height
	// above the sea is bent onto the sphere of the radius plus that height.
	const FVector2D Along = FVector2D(Direction.X, Direction.Y) / Across;
	const double Height = View.Z - SeaLevel;
	const double Up = Height + Radius;
	const double Beyond = FlatRadius / Across;
	const double B = Direction.Z * Up - FlatRadius * Across;
	const double C = FlatRadius * FlatRadius + Up * Up;
	auto Crossing = [B, C](double Sphere, double& OutEnter, double& OutLeave)
	{
		const double Discriminant = B * B - (C - Sphere * Sphere);
		if (Discriminant < 0.0)
		{
			return false;
		}
		const double Root = FMath::Sqrt(Discriminant);
		OutEnter = -B - Root;
		OutLeave = -B + Root;
		return true;
	};

	// Along the ray, from where it is higher than CullAbove to where it goes deeper than CullBelow: below that the
	// bent planet is all there is, and it hides what is behind.
	double Enter = 0.0;
	double Leave = 0.0;
	if (!Crossing(Radius + CVarGlobeCullAbove.GetValueOnGameThread() * 100.0, Enter, Leave) || Leave <= Beyond)
	{
		return;
	}
	const double Start = FMath::Max(Beyond, Enter);
	double End = Leave;
	const double Core = Radius - CVarGlobeCullBelow.GetValueOnGameThread() * 100.0;
	if (Core > 0.0 && Crossing(Core, Enter, Leave) && Leave > Start)
	{
		if (Enter <= Start)
		{
			return;
		}
		End = FMath::Min(End, Enter);
	}

	// Unbent as LBAPlanetUnbend (LBAPlanet.ush) does.
	for (int32 Index = 0; Index <= Samples; ++Index)
	{
		const double T = FMath::Lerp(Start, End, double(Index) / Samples);
		const double Out = T * Across - FlatRadius;
		const double Above = Up + T * Direction.Z;
		const double Angle = FMath::Atan2(Out, Above);
		const double PlaceHeight = FMath::Sqrt(Out * Out + Above * Above) - Radius;
		const FVector2D Place = Along * (FlatRadius + Radius * Angle);
		Visit(FVector(Place.X, Place.Y, PlaceHeight - Height));
	}
}

void FLBAPlanetViewExtension::SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView)
{
	// Only the view the bend is centered on (not scene captures).
	const FVector Origin = InView.ViewMatrices.GetViewOrigin();
	if (!IsCulling() || !InView.IsPerspectiveProjection() || FVector::DistSquared(Origin, View) > FMath::Square(100.0))
	{
		return;
	}

	TArray<FVector, TInlineAllocator<Rays * Rays>> Directions;
	const FMatrix& ClipToWorld = InView.ViewMatrices.GetClipToWorld();
	for (int32 Row = 0; Row < Rays; ++Row)
	{
		for (int32 Column = 0; Column < Rays; ++Column)
		{
			const FVector4 Point = ClipToWorld.TransformFVector4(FVector4(-1.0 + 2.0 * Column / (Rays - 1), -1.0 + 2.0 * Row / (Rays - 1), 0.5, 1.0));
			Directions.Add((FVector(Point) / Point.W - Origin).GetSafeNormal());
		}
	}

	// The bend keeps each place's way from the camera (seen from above), so the flat places in view lie between the
	// view's leftmost and rightmost ways: unless it sees straight down or up, where every way is.
	const FVector2D Forward = FVector2D(InView.GetViewDirection()).GetSafeNormal();
	const FVector2D Side(-Forward.Y, Forward.X);
	bool bAround = Forward.IsZero() || InView.ViewFrustum.IntersectPoint(Origin - FVector(0.0, 0.0, 100.0))
		|| InView.ViewFrustum.IntersectPoint(Origin + FVector(0.0, 0.0, 100.0));
	double MinAngle = 0.0;
	double MaxAngle = 0.0;
	for (const FVector& Direction : Directions)
	{
		const FVector2D Way(Direction.X, Direction.Y);
		if (Way.Size() > 1e-3)
		{
			const double Angle = FMath::Atan2(Way | Side, Way | Forward);
			MinAngle = FMath::Min(MinAngle, Angle);
			MaxAngle = FMath::Max(MaxAngle, Angle);
		}
	}
	bAround |= MaxAngle - MinAngle > FMath::DegreesToRadians(170.0);

	// Outward normals: the volume is what is behind all its planes.
	FConvexVolume::FPlaneArray Planes;
	if (!bAround)
	{
		MinAngle -= AngleMargin;
		MaxAngle += AngleMargin;
		auto Toward = [&Forward, &Side](double Angle) { return Forward * FMath::Cos(Angle) + Side * FMath::Sin(Angle); };
		const FVector2D Low = Toward(MinAngle);
		const FVector2D High = Toward(MaxAngle);
		const FVector2D Middle = Toward(0.5 * (MinAngle + MaxAngle));

		// How high and low they are seen from the camera, toward the middle way, and how far out they go.
		double MinRise = UE_DOUBLE_HALF_PI;
		double MaxRise = -UE_DOUBLE_HALF_PI;
		double Reach = FlatRadius;
		for (const FVector& Direction : Directions)
		{
			ForEachFlatPlace(Direction, [&](const FVector& Place)
			{
				const double Ahead = Middle | FVector2D(Place.X, Place.Y);
				const double Rise = FMath::Atan2(Place.Z, Ahead);
				MinRise = FMath::Min(MinRise, Rise);
				MaxRise = FMath::Max(MaxRise, Rise);
				Reach = FMath::Max(Reach, Ahead);
			});
		}
		MinRise -= AngleMargin;
		MaxRise += AngleMargin;

		Planes.Add(FPlane(Origin, FVector(Low.Y, -Low.X, 0.0)));
		Planes.Add(FPlane(Origin, FVector(-High.Y, High.X, 0.0)));
		const FVector Ahead(Middle.X, Middle.Y, 0.0);
		if (MaxRise < FMath::DegreesToRadians(89.9))
		{
			Planes.Add(FPlane(Origin, FVector(0.0, 0.0, FMath::Cos(MaxRise)) - Ahead * FMath::Sin(MaxRise)));
		}
		if (MinRise > FMath::DegreesToRadians(-89.9))
		{
			Planes.Add(FPlane(Origin, Ahead * FMath::Sin(MinRise) - FVector(0.0, 0.0, FMath::Cos(MinRise))));
		}
		Planes.Add(FPlane(Origin + Ahead * (Reach + ReachMargin), Ahead));
	}
	else
	{
		// All around the camera, as far out as the flat places in view go.
		double Reach = FlatRadius;
		for (const FVector& Direction : Directions)
		{
			ForEachFlatPlace(Direction, [&Reach](const FVector& Place) { Reach = FMath::Max(Reach, Place.Size2D()); });
		}
		Reach += ReachMargin;
		Planes.Add(FPlane(Origin + FVector(Reach, 0.0, 0.0), FVector(1.0, 0.0, 0.0)));
		Planes.Add(FPlane(Origin - FVector(Reach, 0.0, 0.0), FVector(-1.0, 0.0, 0.0)));
		Planes.Add(FPlane(Origin + FVector(0.0, Reach, 0.0), FVector(0.0, 1.0, 0.0)));
		Planes.Add(FPlane(Origin - FVector(0.0, Reach, 0.0), FVector(0.0, -1.0, 0.0)));
	}
	InView.ViewFrustum = FConvexVolume(Planes);
}
