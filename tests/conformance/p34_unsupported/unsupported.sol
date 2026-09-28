module p34.unsupported

capability Probe {
    function scalar(value: Option<Result<Text, Bool>>) -> Int64
    effects { probe.scalar<Self> }
}

@entry
public function launch(probe: capability Probe) -> Int64
effects { probe.scalar<probe> }
{
    probe.scalar(some(ok("probe")))
}
