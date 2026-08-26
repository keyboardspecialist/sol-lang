module run.package.main
use run.package.helper.status

@entry
public function launch() -> Int64 effects { pure } {
    return status()
}
