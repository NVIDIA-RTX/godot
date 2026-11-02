/* eslint-disable no-void */
import { dirname } from 'path';
import { mkdir, writeFile } from 'fs/promises';
import * as core from '@actions/core';
import * as github from '@actions/github';
import retry from 'async-retry';
import type { Context } from '@actions/github/lib/context';
import type { HeadersInit } from 'node-fetch';
import fetch from 'node-fetch';

interface GetRepoResult {
  readonly owner: string;
  readonly repo: string;
}

const getRepo = (inputRepoString: string, context: Context): GetRepoResult => {
  if (inputRepoString === '') {
    return { owner: context.repo.owner, repo: context.repo.repo };
  } else {
    const [owner, repo] = inputRepoString.split('/');
    if (typeof owner === 'undefined' || typeof repo === 'undefined')
      throw new Error('Malformed repo');
    return { owner, repo };
  }
};

interface GetReleaseOptions {
  readonly owner: string;
  readonly repo: string;
  readonly version: string;
}

const getRelease = (
  octokit: ReturnType<typeof github.getOctokit>,
  { owner, repo, version }: GetReleaseOptions
) => {
  const tagsMatch = version.match(/^tags\/(.*)$/);
  if (version === 'latest') {
    return octokit.rest.repos.getLatestRelease({ owner, repo });
  } else if (tagsMatch !== null && tagsMatch[1]) {
    return octokit.rest.repos.getReleaseByTag({
      owner,
      repo,
      tag: tagsMatch[1],
    });
  } else {
    return octokit.rest.repos.getRelease({
      owner,
      repo,
      release_id: Math.trunc(Number(version)),
    });
  }
};

type GetReleaseResult = ReturnType<typeof getRelease> extends Promise<infer T>
  ? T
  : never;

type Asset = GetReleaseResult['data']['assets'][0];

interface FetchAssetFileOptions {
  readonly id: number;
  readonly outputPath: string;
  readonly owner: string;
  readonly repo: string;
  readonly token: string;
}

const baseFetchAssetFile = async (
  octokit: ReturnType<typeof github.getOctokit>,
  { id, outputPath, owner, repo, token }: FetchAssetFileOptions
) => {
  const {
    body,
    headers: { accept, 'user-agent': userAgent },
    method,
    url,
  } = octokit.request.endpoint(
    'GET /repos/:owner/:repo/releases/assets/:asset_id',
    {
      asset_id: id,
      headers: {
        accept: 'application/octet-stream',
      },
      owner,
      repo,
    }
  );
  let headers: HeadersInit = {
    accept,
  };
  if (token !== '')
    headers = { ...headers, authorization: `token ${token}` };

  if (typeof userAgent !== 'undefined')
    headers = { ...headers, 'user-agent': userAgent };

  const response = await fetch(url, { body, headers, method });
  if (!response.ok) {
    const text = await response.text();
    core.warning(text);
    throw new Error('Invalid response');
  }
  const blob = await response.blob();
  const arrayBuffer = await blob.arrayBuffer();
  await mkdir(dirname(outputPath), { recursive: true });
  void (await writeFile(outputPath, new Uint8Array(arrayBuffer)));
};

const fetchAssetFile = (
  octokit: ReturnType<typeof github.getOctokit>,
  options: FetchAssetFileOptions
) =>
  retry(() => baseFetchAssetFile(octokit, options), {
    retries: 5,
    minTimeout: 1000,
  });

const printOutput = (release: GetReleaseResult): void => {
  core.setOutput('version', release.data.tag_name);
  core.setOutput('name', release.data.name);
  core.setOutput('body', release.data.body);
};

const filterByFileName = (file: string) => (asset: Asset) =>
  file === asset.name;

const filterByRegex = (file: string) => (asset: Asset) =>
  new RegExp(file).test(asset.name);

const main = async (): Promise<void> => {
  const { owner, repo } = getRepo(
    core.getInput('repo', { required: false }),
    github.context
  );
  const token = core.getInput('token', { required: false });
  const version = core.getInput('version', { required: false }) || 'latest';
  const inputTarget = core.getInput('target', { required: false });
  const file = core.getInput('file', { required: true });
  const usesRegex = core.getBooleanInput('regex', { required: false });
  const target = inputTarget === '' ? file : inputTarget;
  const baseUrl =
    core.getInput('octokitBaseUrl', { required: false }) || undefined;

  const octokit = github.getOctokit(token, { baseUrl });
  const release = await getRelease(octokit, { owner, repo, version });

  const assetFilterFn = usesRegex
    ? filterByRegex(file)
    : filterByFileName(file);

  const assets = release.data.assets.filter(assetFilterFn);
  if (assets.length === 0) throw new Error('Could not find asset id');
  for (const asset of assets) {
    await fetchAssetFile(octokit, {
      id: asset.id,
      outputPath: usesRegex ? `${target}${asset.name}` : target,
      owner,
      repo,
      token,
    });
  }
  printOutput(release);
};

void main();
