module conformance.p43_capability_meter

capability Meter {
    function tick() -> Int64 effects { meter.tick<Self> }
}
