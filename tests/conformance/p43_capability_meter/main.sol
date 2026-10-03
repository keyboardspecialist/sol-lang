module conformance.p43_capability_meter

@entry
public function launch(meter: capability Meter) -> Int64
effects { meter.tick<meter> }
{
    return meter.tick() + meter.tick() + meter.tick()
}
