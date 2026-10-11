export function validatePublication(pkg, readme) {
  if (pkg.private || pkg.name !== 'openclaw-kubik' ||
      pkg.repository?.url !== 'git+https://github.com/pfrankov/kubik.git' ||
      pkg.publishConfig?.access !== 'public') throw Error('Unexpected publication metadata');
  if (/[\u0400-\u04ff]/u.test(readme)) throw Error('The npm README must be in English');
}

export async function needsPublication(response, integrity) {
  if (response.status === 404) return true;
  if (!response.ok) throw Error(`npm registry lookup failed (${response.status})`);
  const existing = await response.json();
  if (existing.dist?.integrity !== integrity)
    throw Error('This npm version already exists with different contents; bump the package version');
  return false;
}
