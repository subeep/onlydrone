"""Audit final binary/source archives against this working tree and built executables."""
from pathlib import Path
import hashlib,json,zipfile
root=Path.cwd();dist=root/'dist';package=dist/'punjdrone-windows-x64'
sha=lambda b:hashlib.sha256(b).hexdigest()
manifest=json.loads((package/'BUILD_MANIFEST.json').read_text())
for line in (dist/'SHA256SUMS.txt').read_text().splitlines():
 digest,name=line.split(maxsplit=1);assert sha((dist/name).read_bytes())==digest,name
assert sha((dist/'punjdrone-windows-source.zip').read_bytes())==manifest['snapshot_sha256']
for name,digest in manifest['binaries'].items():
 contents=(package/name).read_bytes()
 assert sha(contents)==digest,name
 assert contents==(root/'build/windows'/Path(name).name).read_bytes(),name
with zipfile.ZipFile(dist/'punjdrone-windows-source.zip') as src, zipfile.ZipFile(dist/'punjdrone-windows-x64.zip') as binary:
 assert src.testzip() is None and binary.testzip() is None
 hashes=json.loads(src.read('punjdrone-source/SOURCE_SHA256.json'))
 for name,digest in hashes.items():
  contents=src.read('punjdrone-source/'+name);assert sha(contents)==digest,name
  current=Path('/tmp/punjdrone-win-toolchain')/Path(name).name if name.startswith('dependencies/') else root/name
  assert contents==current.read_bytes(),name
 for name in ['WINDOWS.md','WINDOWS_RUN_GUIDE.md','AUTOMATIC_SWEEP.md','O4_DETECTOR.md','PACKET_MAP.md','FINAL_VERIFICATION.md']:
  assert binary.read('punjdrone-windows-x64/'+name)==(root/'docs'/name).read_bytes(),name
 for path in package.rglob('*'):
  if path.is_file():assert binary.read(str(path.relative_to(dist)))==path.read_bytes(),str(path)
 assert binary.read('punjdrone-windows-x64/licenses/stb_image.txt')==(root/'third_party/stb/LICENSE').read_bytes()
results=dict(passed=True,binaries_match_build=len(manifest['binaries']),source_files_match=len(hashes),zip_crc='passed',sha256='passed',maps_and_final_report_packaged=True,archives={name:sha((dist/name).read_bytes()) for name in ['punjdrone-windows-x64.zip','punjdrone-windows-source.zip']})
(root/'data/analysis/final_20261029/package-audit.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
