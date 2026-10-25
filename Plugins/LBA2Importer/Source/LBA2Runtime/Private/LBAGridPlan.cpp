#include "LBAGridPlan.h"

#include "LBA2Settings.h"
#include "LBATypes.h"

const FIntPoint FLBAGridPlan::Sides[4] = { FIntPoint(-1, 0), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(0, 1) };

ELBAColumn FLBAGridPlanLevel::GetColumn(const FIntPoint& Column) const
{
	const int32 Index = FLBAGridPlan::ToColumnIndex(Column);
	return FLBAGridPlan::IsInGrid(Column) && Columns.IsValidIndex(Index) ? Columns[Index] : ELBAColumn::Other;
}

ELBAWallRule FLBAGridPlanLevel::GetRule(const FIntPoint& Column) const
{
	const int32 Index = FLBAGridPlan::ToColumnIndex(Column);
	return FLBAGridPlan::IsInGrid(Column) && Rules.IsValidIndex(Index) ? Rules[Index] : ELBAWallRule::None;
}

const FLBAGridPlanLevel* FLBAGridPlan::FindLevel(int32 Height) const
{
	return Levels.FindByPredicate([Height](const FLBAGridPlanLevel& Level) { return Level.Height == Height; });
}

int32 FLBAGridPlan::FindRoom(int32 Level, const FIntPoint& Column) const
{
	return Rooms.IndexOfByPredicate([Level, &Column](const FLBAGridPlanRoom& Room) { return Room.Level == Level && Room.Contains(Column); });
}

int32 FLBAGridPlan::FindConnector(int32 Level, const FIntPoint& Column) const
{
	const FIntVector Key(Column.X, Level, Column.Y);
	return Connectors.IndexOfByPredicate([&Key](const FLBAGridPlanConnector& Connector) { return Connector.Columns.Contains(Key); });
}

FVector FLBAGridPlan::CellToWorld(double X, double Y, double Z)
{
	return LBA::ToUnreal(X * LBA::BrickSizeXZ, Y * LBA::BrickSizeY, Z * LBA::BrickSizeXZ, ULBA2Settings::GetWorldScale());
}

FVector FLBAGridPlan::WorldToCell(const FVector& World)
{
	const double Scale = ULBA2Settings::GetWorldScale();
	return FVector(World.X / (LBA::BrickSizeXZ * Scale), World.Z / (LBA::BrickSizeY * Scale), World.Y / (LBA::BrickSizeXZ * Scale));
}
