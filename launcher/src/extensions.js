// Pages added next to Refract's own. Each entry is { id, label, icon, Page }; the sidebar and main.jsx read this list,
// so a new page is one file plus one line here and the shared files stay as they are.
import { PackageOpen } from 'lucide-react';
import { UnknownSources } from './pages/unknown-sources';

export const extensionPages = [
  { id: 'unknown-sources', label: 'Unknown sources', icon: PackageOpen, Page: UnknownSources },
];
